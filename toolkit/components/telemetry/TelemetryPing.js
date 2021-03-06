/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Initial Developer of the Original Code is
 * the Mozilla Foundation
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Taras Glek <tglek@mozilla.com>
 *   Vladan Djeric <vdjeric@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/LightweightThemeManager.jsm");

// When modifying the payload in incompatible ways, please bump this version number
const PAYLOAD_VERSION = 1;

const PREF_SERVER = "toolkit.telemetry.server";
const PREF_ENABLED = "toolkit.telemetry.enabled";
// Do not gather data more than once a minute
const TELEMETRY_INTERVAL = 60000;
// Delay before intializing telemetry (ms)
const TELEMETRY_DELAY = 60000;
// about:memory values to turn into histograms
const MEM_HISTOGRAMS = {
  "js-gc-heap": "MEMORY_JS_GC_HEAP",
  "js-compartments-system": "MEMORY_JS_COMPARTMENTS_SYSTEM",
  "js-compartments-user": "MEMORY_JS_COMPARTMENTS_USER",
  "explicit": "MEMORY_EXPLICIT",
  "resident": "MEMORY_RESIDENT",
  "storage-sqlite": "MEMORY_STORAGE_SQLITE",
  "explicit/images/content/used/uncompressed":
    "MEMORY_IMAGES_CONTENT_USED_UNCOMPRESSED",
  "heap-allocated": "MEMORY_HEAP_ALLOCATED",
  "page-faults-hard": "PAGE_FAULTS_HARD",
  "low-memory-events-virtual": "LOW_MEMORY_EVENTS_VIRTUAL",
  "low-memory-events-physical": "LOW_MEMORY_EVENTS_PHYSICAL"
};
// Seconds of idle time before pinging.
// On idle-daily a gather-telemetry notification is fired, during it probes can
// start asynchronous tasks to gather data.  On the next idle the data is sent.
const IDLE_TIMEOUT_SECONDS = 5 * 60;

var gLastMemoryPoll = null;

function getLocale() {
  return Cc["@mozilla.org/chrome/chrome-registry;1"].
         getService(Ci.nsIXULChromeRegistry).
         getSelectedLocale('global');
}

XPCOMUtils.defineLazyServiceGetter(this, "Telemetry",
                                   "@mozilla.org/base/telemetry;1",
                                   "nsITelemetry");
XPCOMUtils.defineLazyServiceGetter(this, "idleService",
                                   "@mozilla.org/widget/idleservice;1",
                                   "nsIIdleService");

function generateUUID() {
  let str = Cc["@mozilla.org/uuid-generator;1"].getService(Ci.nsIUUIDGenerator).generateUUID().toString();
  // strip {}
  return str.substring(1, str.length - 1);
}

/**
 * Gets a series of simple measurements (counters). At the moment, this
 * only returns startup data from nsIAppStartup.getStartupInfo().
 * 
 * @return simple measurements as a dictionary.
 */
function getSimpleMeasurements() {
  let si = Services.startup.getStartupInfo();

  var ret = {
    // uptime in minutes
    uptime: Math.round((new Date() - si.process) / 60000)
  }

  if (si.process) {
    for each (let field in ["main", "firstPaint", "sessionRestored"]) {
      if (!(field in si))
        continue;
      ret[field] = si[field] - si.process
    }
  }
  ret.startupInterrupted = new Number(Services.startup.interrupted);

  ret.js = Cc["@mozilla.org/js/xpc/XPConnect;1"]
           .getService(Ci.nsIJSEngineTelemetryStats)
           .telemetryValue;

  return ret;
}

function TelemetryPing() {}

TelemetryPing.prototype = {
  _histograms: {},
  _initialized: false,
  _prevValues: {},
  _sqliteOverhead: {},

  /**
   * Returns a set of histograms that can be converted into JSON
   * @return a snapshot of the histograms of form:
   *  { histogram_name: {range:[minvalue,maxvalue], bucket_count:<number of buckets>,
   *    histogram_type: <0 for exponential, 1 for linear>, bucketX:countX, ....} ...}
   * where bucket[XY], count[XY] are positive integers.
   */
  getHistograms: function getHistograms() {
    let hls = Telemetry.histogramSnapshots;
    let ret = {};

    // bug 701583: report sqlite overhead on startup
    for (let key in this._sqliteOverhead) {
      hls[key] = this._sqliteOverhead[key];
    }

    for (let key in hls) {
      let hgram = hls[key];
      if (!hgram.static)
        continue;

      let r = hgram.ranges;
      let c = hgram.counts;
      let retgram = {
        range: [r[1], r[r.length - 1]],
        bucket_count: r.length,
        histogram_type: hgram.histogram_type,
        values: {},
        sum: hgram.sum
      };
      let first = true;
      let last = 0;

      for (let i = 0; i < c.length; i++) {
        let value = c[i];
        if (!value)
          continue;

        // add a lower bound
        if (i && first) {
          first = false;
          retgram.values[r[i - 1]] = 0;
        }
        first = false;
        last = i + 1;
        retgram.values[r[i]] = value;
      }

      // add an upper bound
      if (last && last < c.length)
        retgram.values[r[last]] = 0;
      ret[key] = retgram;
    }
    return ret;
  },

  addValue: function addValue(name, id, val) {
    let h = this._histograms[name];
    if (!h) {
      h = Telemetry.getHistogramById(id);
      this._histograms[name] = h;
    }
    h.add(val);
  },

  /**
   * Descriptive metadata
   * 
   * @param  reason
   *         The reason for the telemetry ping, this will be included in the
   *         returned metadata,
   * @return The metadata as a JS object
   */
  getMetadata: function getMetadata(reason) {
    let ai = Services.appinfo;
    let ret = {
      reason: reason,
      OS: ai.OS,
      appID: ai.ID,
      appVersion: ai.version,
      appName: ai.name,
      appBuildID: ai.appBuildID,
      platformBuildID: ai.platformBuildID,
    };

    // sysinfo fields are not always available, get what we can.
    let sysInfo = Cc["@mozilla.org/system-info;1"].getService(Ci.nsIPropertyBag2);
    let fields = ["cpucount", "memsize", "arch", "version", "device", "manufacturer", "hardware",
                  "hasMMX", "hasSSE", "hasSSE2", "hasSSE3",
                  "hasSSSE3", "hasSSE4A", "hasSSE4_1", "hasSSE4_2",
                  "hasEDSP", "hasARMv6", "hasNEON"];
    for each (let field in fields) {
      let value;
      try {
        value = sysInfo.getProperty(field);
      } catch (e) {
        continue
      }
      if (field == "memsize") {
        // Send RAM size in megabytes. Rounding because sysinfo doesn't
        // always provide RAM in multiples of 1024.
        value = Math.round(value / 1024 / 1024)
      }
      ret[field] = value
    }

    let theme = LightweightThemeManager.currentTheme;
    if (theme)
      ret.persona = theme.id;

    if (this._addons)
      ret.addons = this._addons;

    return ret;
  },

  /**
   * Pull values from about:memory into corresponding histograms
   */
  gatherMemory: function gatherMemory() {
    let mgr;
    try {
      mgr = Cc["@mozilla.org/memory-reporter-manager;1"].
            getService(Ci.nsIMemoryReporterManager);
    } catch (e) {
      // OK to skip memory reporters in xpcshell
      return;
    }

    let e = mgr.enumerateReporters();
    while (e.hasMoreElements()) {
      let mr = e.getNext().QueryInterface(Ci.nsIMemoryReporter);
      let id = MEM_HISTOGRAMS[mr.path];
      if (!id) {
        continue;
      }
      // mr.amount is expensive to read in some cases, so get it only once.
      let amount = mr.amount;
      if (amount == -1) {
        continue;
      }

      let val;
      if (mr.units == Ci.nsIMemoryReporter.UNITS_BYTES) {
        val = Math.floor(amount / 1024);
      }
      else if (mr.units == Ci.nsIMemoryReporter.UNITS_COUNT) {
        val = amount;
      }
      else if (mr.units == Ci.nsIMemoryReporter.UNITS_COUNT_CUMULATIVE) {
        // If the reporter gives us a cumulative count, we'll report the
        // difference in its value between now and our previous ping.

        if (!(mr.path in this._prevValues)) {
          // If this is the first time we're reading this reporter, store its
          // current value but don't report it in the telemetry ping, so we
          // ignore the effect startup had on the reporter.
          this._prevValues[mr.path] = amount;
          continue;
        }

        val = amount - this._prevValues[mr.path];
        this._prevValues[mr.path] = amount;
      }
      else {
        NS_ASSERT(false, "Can't handle memory reporter with units " + mr.units);
        continue;
      }
      this.addValue(mr.path, id, val);
    }
  },
  
  /** 
   * Make a copy of sqlite histograms on startup
   */
  gatherStartupSqlite: function gatherStartupSqlite() {
    let hls = Telemetry.histogramSnapshots;
    let sqlite_re = /SQLITE/;
    for (let key in hls) {
      if (sqlite_re.test(key))
        this._sqliteOverhead["STARTUP_" + key] = hls[key];
    }
  },

  /**
   * Send data to the server. Record success/send-time in histograms
   */
  send: function send(reason, server) {
    // populate histograms one last time
    this.gatherMemory();
    let payload = {
      ver: PAYLOAD_VERSION,
      info: this.getMetadata(reason),
      simpleMeasurements: getSimpleMeasurements(),
      histograms: this.getHistograms(),
      slowSQL: Telemetry.slowSQL
    };

    let isTestPing = (reason == "test-ping");
    // Generate a unique id once per session so the server can cope with duplicate submissions.
    // Use a deterministic url for testing.
    if (!this._path)
      this._path = "/submit/telemetry/" + (isTestPing ? reason : generateUUID());
    
    let hping = Telemetry.getHistogramById("TELEMETRY_PING");
    let hsuccess = Telemetry.getHistogramById("TELEMETRY_SUCCESS");

    let url = server + this._path;
    let request = Cc["@mozilla.org/xmlextras/xmlhttprequest;1"]
                  .createInstance(Ci.nsIXMLHttpRequest);
    request.mozBackgroundRequest = true;
    request.open("POST", url, true);
    request.overrideMimeType("text/plain");
    request.setRequestHeader("Content-Type", "application/json");

    let startTime = new Date();

    function finishRequest(channel) {
      let success = false;
      try {
        success = channel.QueryInterface(Ci.nsIHttpChannel).requestSucceeded;
      } catch(e) {
      }
      hsuccess.add(success);
      hping.add(new Date() - startTime);
      if (isTestPing)
        Services.obs.notifyObservers(null, "telemetry-test-xhr-complete", null);
    }
    request.addEventListener("error", function(aEvent) finishRequest(request.channel), false);
    request.addEventListener("load", function(aEvent) finishRequest(request.channel), false);

    request.send(JSON.stringify(payload));
  },
  
  attachObservers: function attachObservers() {
    if (!this._initialized)
      return;
    Services.obs.addObserver(this, "cycle-collector-begin", false);
    Services.obs.addObserver(this, "idle-daily", false);
  },

  detachObservers: function detachObservers() {
    if (!this._initialized)
      return;
    Services.obs.removeObserver(this, "idle-daily");
    Services.obs.removeObserver(this, "cycle-collector-begin");
    if (this._isIdleObserver) {
      idleService.removeIdleObserver(this, IDLE_TIMEOUT_SECONDS);
      this._isIdleObserver = false;
    }
  },

  /**
   * Initializes telemetry within a timer. If there is no PREF_SERVER set, don't turn on telemetry.
   */
  setup: function setup() {
    let enabled = false; 
    try {
      enabled = Services.prefs.getBoolPref(PREF_ENABLED);
      this._server = Services.prefs.getCharPref(PREF_SERVER);
    } catch (e) {
      // Prerequesite prefs aren't set
    }
    if (!enabled) {
      // Turn off local telemetry if telemetry is disabled.
      // This may change once about:telemetry is added.
      Telemetry.canRecord = false;
      return;
    }
    Services.obs.addObserver(this, "private-browsing", false);
    Services.obs.addObserver(this, "profile-before-change", false);
    Services.obs.addObserver(this, "sessionstore-windows-restored", false);

    // Delay full telemetry initialization to give the browser time to
    // run various late initializers. Otherwise our gathered memory
    // footprint and other numbers would be too optimistic.
    let self = this;
    this._timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    let timerCallback = function() {
      self._initialized = true;
      self.attachObservers();
      self.gatherMemory();
      delete self._timer
    }
    this._timer.initWithCallback(timerCallback, TELEMETRY_DELAY, Ci.nsITimer.TYPE_ONE_SHOT);
  },

  /** 
   * Remove observers to avoid leaks
   */
  uninstall: function uninstall() {
    this.detachObservers()
    Services.obs.removeObserver(this, "sessionstore-windows-restored");
    Services.obs.removeObserver(this, "profile-before-change");
    Services.obs.removeObserver(this, "private-browsing");
  },

  /**
   * This observer drives telemetry.
   */
  observe: function (aSubject, aTopic, aData) {
    // Allows to change the server for testing
    var server = this._server;

    switch (aTopic) {
    case "Add-ons":
      this._addons = aData;
      break;
    case "profile-after-change":
      this.setup();
      break;
    case "profile-before-change":
      this.uninstall();
      break;
    case "cycle-collector-begin":
      let now = new Date();
      if (!gLastMemoryPoll
          || (TELEMETRY_INTERVAL <= now - gLastMemoryPoll)) {
        gLastMemoryPoll = now;
        this.gatherMemory();
      }
      break;
    case "private-browsing":
      Telemetry.canRecord = aData == "exit";
      if (aData == "enter") {
        this.detachObservers()
      } else {
        this.attachObservers()
      }
      break;
    case "sessionstore-windows-restored":
      this.gatherStartupSqlite();
      break;
    case "idle-daily":
      // Enqueue to main-thread, otherwise components may be inited by the
      // idle-daily category and miss the gather-telemetry notification.
      Services.tm.mainThread.dispatch((function() {
        // Notify that data should be gathered now, since ping will happen soon.
        Services.obs.notifyObservers(null, "gather-telemetry", null);
        // The ping happens at the first idle of length IDLE_TIMEOUT_SECONDS.
        idleService.addIdleObserver(this, IDLE_TIMEOUT_SECONDS);
        this._isIdleObserver = true;
      }).bind(this), Ci.nsIThread.DISPATCH_NORMAL);
      break;
    case "test-ping":
      server = aData;
      // fall through
    case "idle":
      if (this._isIdleObserver) {
        idleService.removeIdleObserver(this, IDLE_TIMEOUT_SECONDS);
        this._isIdleObserver = false;
      }
      this.send(aTopic == "idle" ? "idle-daily" : aTopic, server);
      break;
    }
  },

  classID: Components.ID("{55d6a5fa-130e-4ee6-a158-0133af3b86ba}"),
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIObserver]),
};

let NSGetFactory = XPCOMUtils.generateNSGetFactory([TelemetryPing]);
