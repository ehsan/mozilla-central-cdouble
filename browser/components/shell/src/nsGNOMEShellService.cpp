/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
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
 * The Original Code is Shell Service.
 *
 * The Initial Developer of the Original Code is mozilla.org.
 * Portions created by the Initial Developer are Copyright (C) 2004
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
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

#include "mozilla/Util.h"

#include "nsCOMPtr.h"
#include "nsGNOMEShellService.h"
#include "nsShellService.h"
#include "nsIServiceManager.h"
#include "nsILocalFile.h"
#include "nsIProperties.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIPrefService.h"
#include "prenv.h"
#include "nsStringAPI.h"
#include "nsIGConfService.h"
#include "nsIGIOService.h"
#include "nsIGSettingsService.h"
#include "nsIStringBundle.h"
#include "nsIOutputStream.h"
#include "nsIProcess.h"
#include "nsNetUtil.h"
#include "nsIDOMHTMLImageElement.h"
#include "nsIImageLoadingContent.h"
#include "imgIRequest.h"
#include "imgIContainer.h"
#include "prprf.h"
#ifdef MOZ_WIDGET_GTK2
#include "nsIImageToPixbuf.h"
#endif

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits.h>
#include <stdlib.h>

using namespace mozilla;

struct ProtocolAssociation
{
  const char *name;
  bool essential;
};

struct MimeTypeAssociation
{
  const char *mimeType;
  const char *extensions;
};

static const ProtocolAssociation appProtocols[] = {
  { "http",   true     },
  { "https",  true     },
  { "ftp",    false },
  { "chrome", false }
};

static const MimeTypeAssociation appTypes[] = {
  { "text/html",             "htm html shtml" },
  { "application/xhtml+xml", "xhtml xht"      }
};

static const char kDocumentIconPath[] = "firefox-document.png";

// GConf registry key constants
#define DG_BACKGROUND "/desktop/gnome/background"

static const char kDesktopImageKey[] = DG_BACKGROUND "/picture_filename";
static const char kDesktopOptionsKey[] = DG_BACKGROUND "/picture_options";
static const char kDesktopDrawBGKey[] = DG_BACKGROUND "/draw_background";
static const char kDesktopColorKey[] = DG_BACKGROUND "/primary_color";

static const char kDesktopBGSchema[] = "org.gnome.desktop.background";
static const char kDesktopImageGSKey[] = "picture-uri";
static const char kDesktopOptionGSKey[] = "picture-options";
static const char kDesktopDrawBGGSKey[] = "draw-background";
static const char kDesktopColorGSKey[] = "primary-color";

nsresult
nsGNOMEShellService::Init()
{
  nsresult rv;

  // GConf, GSettings or GIO _must_ be available, or we do not allow
  // CreateInstance to succeed.

  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);
  nsCOMPtr<nsIGIOService> giovfs =
    do_GetService(NS_GIOSERVICE_CONTRACTID);
  nsCOMPtr<nsIGSettingsService> gsettings =
    do_GetService(NS_GSETTINGSSERVICE_CONTRACTID);

  if (!gconf && !giovfs && !gsettings)
    return NS_ERROR_NOT_AVAILABLE;

  // Check G_BROKEN_FILENAMES.  If it's set, then filenames in glib use
  // the locale encoding.  If it's not set, they use UTF-8.
  mUseLocaleFilenames = PR_GetEnv("G_BROKEN_FILENAMES") != nsnull;

  if (GetAppPathFromLauncher())
    return NS_OK;

  nsCOMPtr<nsIProperties> dirSvc
    (do_GetService("@mozilla.org/file/directory_service;1"));
  NS_ENSURE_TRUE(dirSvc, NS_ERROR_NOT_AVAILABLE);

  nsCOMPtr<nsILocalFile> appPath;
  rv = dirSvc->Get(NS_XPCOM_CURRENT_PROCESS_DIR, NS_GET_IID(nsILocalFile),
                   getter_AddRefs(appPath));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = appPath->AppendNative(NS_LITERAL_CSTRING(MOZ_APP_NAME));
  NS_ENSURE_SUCCESS(rv, rv);

  return appPath->GetNativePath(mAppPath);
}

NS_IMPL_ISUPPORTS1(nsGNOMEShellService, nsIShellService)

bool
nsGNOMEShellService::GetAppPathFromLauncher()
{
  gchar *tmp;

  const char *launcher = PR_GetEnv("MOZ_APP_LAUNCHER");
  if (!launcher)
    return false;

  if (g_path_is_absolute(launcher)) {
    mAppPath = launcher;
    tmp = g_path_get_basename(launcher);
    gchar *fullpath = g_find_program_in_path(tmp);
    if (fullpath && mAppPath.Equals(fullpath))
      mAppIsInPath = true;
    g_free(fullpath);
  } else {
    tmp = g_find_program_in_path(launcher);
    if (!tmp)
      return false;
    mAppPath = tmp;
    mAppIsInPath = true;
  }

  g_free(tmp);
  return true;
}

bool
nsGNOMEShellService::KeyMatchesAppName(const char *aKeyValue) const
{

  gchar *commandPath;
  if (mUseLocaleFilenames) {
    gchar *nativePath = g_filename_from_utf8(aKeyValue, -1, NULL, NULL, NULL);
    if (!nativePath) {
      NS_ERROR("Error converting path to filesystem encoding");
      return false;
    }

    commandPath = g_find_program_in_path(nativePath);
    g_free(nativePath);
  } else {
    commandPath = g_find_program_in_path(aKeyValue);
  }

  if (!commandPath)
    return false;

  bool matches = mAppPath.Equals(commandPath);
  g_free(commandPath);
  return matches;
}

bool
nsGNOMEShellService::CheckHandlerMatchesAppName(const nsACString &handler) const
{
  gint argc;
  gchar **argv;
  nsCAutoString command(handler);

  // The string will be something of the form: [/path/to/]browser "%s"
  // We want to remove all of the parameters and get just the binary name.

  if (g_shell_parse_argv(command.get(), &argc, &argv, NULL) && argc > 0) {
    command.Assign(argv[0]);
    g_strfreev(argv);
  }

  if (!KeyMatchesAppName(command.get()))
    return false; // the handler is set to another app

  return true;
}

NS_IMETHODIMP
nsGNOMEShellService::IsDefaultBrowser(bool aStartupCheck,
                                      bool* aIsDefaultBrowser)
{
  *aIsDefaultBrowser = false;
  if (aStartupCheck)
    mCheckedThisSession = true;

  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);
  nsCOMPtr<nsIGIOService> giovfs = do_GetService(NS_GIOSERVICE_CONTRACTID);

  bool enabled;
  nsCAutoString handler;
  nsCOMPtr<nsIGIOMimeApp> gioApp;

  for (unsigned int i = 0; i < ArrayLength(appProtocols); ++i) {
    if (!appProtocols[i].essential)
      continue;

    if (gconf) {
      handler.Truncate();
      gconf->GetAppForProtocol(nsDependentCString(appProtocols[i].name),
                               &enabled, handler);

      if (!CheckHandlerMatchesAppName(handler) || !enabled)
        return NS_OK; // the handler is disabled or set to another app
    }

    if (giovfs) {
      handler.Truncate();
      giovfs->GetAppForURIScheme(nsDependentCString(appProtocols[i].name),
                                 getter_AddRefs(gioApp));
      if (!gioApp)
        return NS_OK;

      gioApp->GetCommand(handler);

      if (!CheckHandlerMatchesAppName(handler))
        return NS_OK; // the handler is set to another app
    }
  }

  *aIsDefaultBrowser = true;

  return NS_OK;
}

NS_IMETHODIMP
nsGNOMEShellService::SetDefaultBrowser(bool aClaimAllTypes,
                                       bool aForAllUsers)
{
#ifdef DEBUG
  if (aForAllUsers)
    NS_WARNING("Setting the default browser for all users is not yet supported");
#endif

  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);
  nsCOMPtr<nsIGIOService> giovfs = do_GetService(NS_GIOSERVICE_CONTRACTID);
  if (gconf) {
    nsCAutoString appKeyValue;
    if (mAppIsInPath) {
      // mAppPath is in the users path, so use only the basename as the launcher
      gchar *tmp = g_path_get_basename(mAppPath.get());
      appKeyValue = tmp;
      g_free(tmp);
    } else {
      appKeyValue = mAppPath;
    }

    appKeyValue.AppendLiteral(" %s");

    for (unsigned int i = 0; i < ArrayLength(appProtocols); ++i) {
      if (appProtocols[i].essential || aClaimAllTypes) {
        gconf->SetAppForProtocol(nsDependentCString(appProtocols[i].name),
                                 appKeyValue);
      }
    }
  }

  if (giovfs) {
    nsresult rv;
    nsCOMPtr<nsIStringBundleService> bundleService =
      do_GetService(NS_STRINGBUNDLE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIStringBundle> brandBundle;
    rv = bundleService->CreateBundle(BRAND_PROPERTIES, getter_AddRefs(brandBundle));
    NS_ENSURE_SUCCESS(rv, rv);

    nsString brandShortName;
    brandBundle->GetStringFromName(NS_LITERAL_STRING("brandShortName").get(),
                                   getter_Copies(brandShortName));

    // use brandShortName as the application id.
    NS_ConvertUTF16toUTF8 id(brandShortName);
    nsCOMPtr<nsIGIOMimeApp> appInfo;
    rv = giovfs->CreateAppFromCommand(mAppPath,
                                      id,
                                      getter_AddRefs(appInfo));
    NS_ENSURE_SUCCESS(rv, rv);

    // set handler for the protocols
    for (unsigned int i = 0; i < ArrayLength(appProtocols); ++i) {
      if (appProtocols[i].essential || aClaimAllTypes) {
        appInfo->SetAsDefaultForURIScheme(nsDependentCString(appProtocols[i].name));
      }
    }

    // set handler for .html and xhtml files and MIME types:
    if (aClaimAllTypes) {
      // Add mime types for html, xhtml extension and set app to just created appinfo.
      for (unsigned int i = 0; i < ArrayLength(appTypes); ++i) {
        appInfo->SetAsDefaultForMimeType(nsDependentCString(appTypes[i].mimeType));
        appInfo->SetAsDefaultForFileExtensions(nsDependentCString(appTypes[i].extensions));
      }
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsGNOMEShellService::GetShouldCheckDefaultBrowser(bool* aResult)
{
  // If we've already checked, the browser has been started and this is a 
  // new window open, and we don't want to check again.
  if (mCheckedThisSession) {
    *aResult = false;
    return NS_OK;
  }

  nsCOMPtr<nsIPrefBranch> prefs;
  nsCOMPtr<nsIPrefService> pserve(do_GetService(NS_PREFSERVICE_CONTRACTID));
  if (pserve)
    pserve->GetBranch("", getter_AddRefs(prefs));

  if (prefs)
    prefs->GetBoolPref(PREF_CHECKDEFAULTBROWSER, aResult);

  return NS_OK;
}

NS_IMETHODIMP
nsGNOMEShellService::SetShouldCheckDefaultBrowser(bool aShouldCheck)
{
  nsCOMPtr<nsIPrefBranch> prefs;
  nsCOMPtr<nsIPrefService> pserve(do_GetService(NS_PREFSERVICE_CONTRACTID));
  if (pserve)
    pserve->GetBranch("", getter_AddRefs(prefs));

  if (prefs)
    prefs->SetBoolPref(PREF_CHECKDEFAULTBROWSER, aShouldCheck);

  return NS_OK;
}

static nsresult
WriteImage(const nsCString& aPath, imgIContainer* aImage)
{
#ifndef MOZ_WIDGET_GTK2
  return NS_ERROR_NOT_AVAILABLE;
#else
  nsCOMPtr<nsIImageToPixbuf> imgToPixbuf =
    do_GetService("@mozilla.org/widget/image-to-gdk-pixbuf;1");
  if (!imgToPixbuf)
      return NS_ERROR_NOT_AVAILABLE;

  GdkPixbuf* pixbuf = imgToPixbuf->ConvertImageToPixbuf(aImage);
  if (!pixbuf)
      return NS_ERROR_NOT_AVAILABLE;

  gboolean res = gdk_pixbuf_save(pixbuf, aPath.get(), "png", NULL, NULL);

  g_object_unref(pixbuf);
  return res ? NS_OK : NS_ERROR_FAILURE;
#endif
}
                 
NS_IMETHODIMP
nsGNOMEShellService::SetDesktopBackground(nsIDOMElement* aElement, 
                                          PRInt32 aPosition)
{
  nsresult rv;
  nsCOMPtr<nsIImageLoadingContent> imageContent = do_QueryInterface(aElement, &rv);
  if (!imageContent) return rv;

  // get the image container
  nsCOMPtr<imgIRequest> request;
  rv = imageContent->GetRequest(nsIImageLoadingContent::CURRENT_REQUEST,
                                getter_AddRefs(request));
  if (!request) return rv;
  nsCOMPtr<imgIContainer> container;
  rv = request->GetImage(getter_AddRefs(container));
  if (!container) return rv;

  // Set desktop wallpaper filling style
  nsCAutoString options;
  if (aPosition == BACKGROUND_TILE)
    options.Assign("wallpaper");
  else if (aPosition == BACKGROUND_STRETCH)
    options.Assign("stretched");
  else
    options.Assign("centered");

  // Write the background file to the home directory.
  nsCAutoString filePath(PR_GetEnv("HOME"));

  // get the product brand name from localized strings
  nsString brandName;
  nsCID bundleCID = NS_STRINGBUNDLESERVICE_CID;
  nsCOMPtr<nsIStringBundleService> bundleService(do_GetService(bundleCID));
  if (bundleService) {
    nsCOMPtr<nsIStringBundle> brandBundle;
    rv = bundleService->CreateBundle(BRAND_PROPERTIES,
                                     getter_AddRefs(brandBundle));
    if (NS_SUCCEEDED(rv) && brandBundle) {
      rv = brandBundle->GetStringFromName(NS_LITERAL_STRING("brandShortName").get(),
                                          getter_Copies(brandName));
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  // build the file name
  filePath.Append('/');
  filePath.Append(NS_ConvertUTF16toUTF8(brandName));
  filePath.Append("_wallpaper.png");

  // write the image to a file in the home dir
  rv = WriteImage(filePath, container);
  NS_ENSURE_SUCCESS(rv, rv);

  // Try GSettings first. If we don't have GSettings or the right schema, fall back
  // to using GConf instead. Note that if GSettings works ok, the changes get
  // mirrored to GConf by the gsettings->gconf bridge in gnome-settings-daemon
  nsCOMPtr<nsIGSettingsService> gsettings = 
    do_GetService(NS_GSETTINGSSERVICE_CONTRACTID);
  if (gsettings) {
    nsCOMPtr<nsIGSettingsCollection> background_settings;
    gsettings->GetCollectionForSchema(
      NS_LITERAL_CSTRING(kDesktopBGSchema), getter_AddRefs(background_settings));
    if (background_settings) {
      gchar *file_uri = g_filename_to_uri(filePath.get(), NULL, NULL);
      if (!file_uri)
         return NS_ERROR_FAILURE;

      background_settings->SetString(NS_LITERAL_CSTRING(kDesktopOptionGSKey),
                                     options);

      background_settings->SetString(NS_LITERAL_CSTRING(kDesktopImageGSKey),
                                     nsDependentCString(file_uri));
      g_free(file_uri);
      background_settings->SetBoolean(NS_LITERAL_CSTRING(kDesktopDrawBGGSKey),
                                      true);
      return rv;
    }
  }

  // if the file was written successfully, set it as the system wallpaper
  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);

  if (gconf) {
    gconf->SetString(NS_LITERAL_CSTRING(kDesktopOptionsKey), options);

    // Set the image to an empty string first to force a refresh
    // (since we could be writing a new image on top of an existing
    // Firefox_wallpaper.png and nautilus doesn't monitor the file for changes)
    gconf->SetString(NS_LITERAL_CSTRING(kDesktopImageKey),
                     EmptyCString());

    gconf->SetString(NS_LITERAL_CSTRING(kDesktopImageKey), filePath);
    gconf->SetBool(NS_LITERAL_CSTRING(kDesktopDrawBGKey), true);
  }

  return rv;
}

#define COLOR_16_TO_8_BIT(_c) ((_c) >> 8)
#define COLOR_8_TO_16_BIT(_c) ((_c) << 8 | (_c))

NS_IMETHODIMP
nsGNOMEShellService::GetDesktopBackgroundColor(PRUint32 *aColor)
{
  nsCOMPtr<nsIGSettingsService> gsettings = 
    do_GetService(NS_GSETTINGSSERVICE_CONTRACTID);
  nsCOMPtr<nsIGSettingsCollection> background_settings;
  nsCAutoString background;

  if (gsettings) {
    gsettings->GetCollectionForSchema(
      NS_LITERAL_CSTRING(kDesktopBGSchema), getter_AddRefs(background_settings));
    if (background_settings) {
      background_settings->GetString(NS_LITERAL_CSTRING(kDesktopColorGSKey),
                                     background);
    }
  }

  if (!background_settings) {
    nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);
    if (gconf)
      gconf->GetString(NS_LITERAL_CSTRING(kDesktopColorKey), background);
  }

  if (background.IsEmpty()) {
    *aColor = 0;
    return NS_OK;
  }

  GdkColor color;
  gboolean success = gdk_color_parse(background.get(), &color);

  NS_ENSURE_TRUE(success, NS_ERROR_FAILURE);

  *aColor = COLOR_16_TO_8_BIT(color.red) << 16 |
            COLOR_16_TO_8_BIT(color.green) << 8 |
            COLOR_16_TO_8_BIT(color.blue);
  return NS_OK;
}

static void
ColorToCString(PRUint32 aColor, nsCString& aResult)
{
  // The #rrrrggggbbbb format is used to match gdk_color_to_string()
  char *buf = aResult.BeginWriting(13);
  if (!buf)
    return;

  PRUint16 red = COLOR_8_TO_16_BIT((aColor >> 16) & 0xff);
  PRUint16 green = COLOR_8_TO_16_BIT((aColor >> 8) & 0xff);
  PRUint16 blue = COLOR_8_TO_16_BIT(aColor & 0xff);

  PR_snprintf(buf, 14, "#%04x%04x%04x", red, green, blue);
}

NS_IMETHODIMP
nsGNOMEShellService::SetDesktopBackgroundColor(PRUint32 aColor)
{
  NS_ASSERTION(aColor <= 0xffffff, "aColor has extra bits");
  nsCAutoString colorString;
  ColorToCString(aColor, colorString);

  nsCOMPtr<nsIGSettingsService> gsettings =
    do_GetService(NS_GSETTINGSSERVICE_CONTRACTID);
  if (gsettings) {
    nsCOMPtr<nsIGSettingsCollection> background_settings;
    gsettings->GetCollectionForSchema(
      NS_LITERAL_CSTRING(kDesktopBGSchema), getter_AddRefs(background_settings));
    if (background_settings) {
      background_settings->SetString(NS_LITERAL_CSTRING(kDesktopColorGSKey),
                                     colorString);
      return NS_OK;
    }
  }

  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);

  if (gconf) {
    gconf->SetString(NS_LITERAL_CSTRING(kDesktopColorKey), colorString);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsGNOMEShellService::OpenApplication(PRInt32 aApplication)
{
  nsCAutoString scheme;
  if (aApplication == APPLICATION_MAIL)
    scheme.Assign("mailto");
  else if (aApplication == APPLICATION_NEWS)
    scheme.Assign("news");
  else
    return NS_ERROR_NOT_AVAILABLE;

  nsCOMPtr<nsIGIOService> giovfs = do_GetService(NS_GIOSERVICE_CONTRACTID);
  if (giovfs) {
    nsCOMPtr<nsIGIOMimeApp> gioApp;
    giovfs->GetAppForURIScheme(scheme, getter_AddRefs(gioApp));
    if (gioApp)
      return gioApp->Launch(EmptyCString());
  }

  nsCOMPtr<nsIGConfService> gconf = do_GetService(NS_GCONFSERVICE_CONTRACTID);
  if (!gconf)
    return NS_ERROR_FAILURE;

  bool enabled;
  nsCAutoString appCommand;
  gconf->GetAppForProtocol(scheme, &enabled, appCommand);

  if (!enabled)
    return NS_ERROR_FAILURE;

  // XXX we don't currently handle launching a terminal window.
  // If the handler requires a terminal, bail.
  bool requiresTerminal;
  gconf->HandlerRequiresTerminal(scheme, &requiresTerminal);
  if (requiresTerminal)
    return NS_ERROR_FAILURE;

  // Perform shell argument expansion
  int argc;
  char **argv;
  if (!g_shell_parse_argv(appCommand.get(), &argc, &argv, NULL))
    return NS_ERROR_FAILURE;

  char **newArgv = new char*[argc + 1];
  int newArgc = 0;

  // Run through the list of arguments.  Copy all of them to the new
  // argv except for %s, which we skip.
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "%s") != 0)
      newArgv[newArgc++] = argv[i];
  }

  newArgv[newArgc] = nsnull;

  gboolean err = g_spawn_async(NULL, newArgv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, NULL, NULL);

  g_strfreev(argv);
  delete[] newArgv;

  return err ? NS_OK : NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsGNOMEShellService::OpenApplicationWithURI(nsILocalFile* aApplication, const nsACString& aURI)
{
  nsresult rv;
  nsCOMPtr<nsIProcess> process = 
    do_CreateInstance("@mozilla.org/process/util;1", &rv);
  if (NS_FAILED(rv))
    return rv;
  
  rv = process->Init(aApplication);
  if (NS_FAILED(rv))
    return rv;

  const nsCString spec(aURI);
  const char* specStr = spec.get();
  return process->Run(false, &specStr, 1);
}

NS_IMETHODIMP
nsGNOMEShellService::GetDefaultFeedReader(nsILocalFile** _retval)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}
