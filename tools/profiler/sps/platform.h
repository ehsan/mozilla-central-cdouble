// Copyright (c) 2006-2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef ANDROID
#include <android/log.h>
#else
#define __android_log_print(a, ...)
#endif

#include "mozilla/Util.h"
#include "mozilla/unused.h"
#include "v8-support.h"
#include <vector>
#define ASSERT(a) MOZ_ASSERT(a)
#ifdef ANDROID
#define ENABLE_SPS_LEAF_DATA
#define LOG(text) __android_log_print(ANDROID_LOG_ERROR, "profiler", "%s", text);
#else
#define LOG(text) printf("Profiler: %s\n", text)
#endif

#include <stdint.h>
typedef uint8 byte;
typedef byte* Address;

class MapEntry {
public:
  MapEntry(unsigned long aStart, unsigned long aEnd, unsigned long aOffset, char *aName)
    : mStart(aStart)
    , mEnd(aEnd)
    , mOffset(aOffset)
    , mName(strdup(aName))
  {}

  MapEntry(const MapEntry& aEntry)
    : mStart(aEntry.mStart)
    , mEnd(aEntry.mEnd)
    , mOffset(aEntry.mOffset)
    , mName(strdup(aEntry.mName))
  {}

  ~MapEntry()
  {
    free(mName);
  }

  unsigned long GetStart() { return mStart; }
  unsigned long GetEnd() { return mEnd; }
  char* GetName() { return mName; }

private:
  unsigned long mStart;
  unsigned long mEnd;
  unsigned long mOffset;
  char *mName;
};

class MapInfo {
public:
  MapInfo() {}

  void AddMapEntry(MapEntry entry)
  {
    mEntries.push_back(entry);
  }

  MapEntry& GetEntry(size_t i)
  {
    return mEntries[i];
  }

  size_t GetSize()
  {
    return mEntries.size();
  }
private:
  std::vector<MapEntry> mEntries;
};

#ifdef ENABLE_SPS_LEAF_DATA
struct MapInfo getmaps(pid_t pid);
#endif
// ----------------------------------------------------------------------------
// Mutex
//
// Mutexes are used for serializing access to non-reentrant sections of code.
// The implementations of mutex should allow for nested/recursive locking.

class Mutex {
 public:
  virtual ~Mutex() {}

  // Locks the given mutex. If the mutex is currently unlocked, it becomes
  // locked and owned by the calling thread, and immediately. If the mutex
  // is already locked by another thread, suspends the calling thread until
  // the mutex is unlocked.
  virtual int Lock() = 0;

  // Unlocks the given mutex. The mutex is assumed to be locked and owned by
  // the calling thread on entrance.
  virtual int Unlock() = 0;

  // Tries to lock the given mutex. Returns whether the mutex was
  // successfully locked.
  virtual bool TryLock() = 0;
};

// ----------------------------------------------------------------------------
// ScopedLock
//
// Stack-allocated ScopedLocks provide block-scoped locking and
// unlocking of a mutex.
class ScopedLock {
 public:
  explicit ScopedLock(Mutex* mutex): mutex_(mutex) {
    ASSERT(mutex_ != NULL);
    mutex_->Lock();
  }
  ~ScopedLock() {
    mutex_->Unlock();
  }

 private:
  Mutex* mutex_;
  DISALLOW_COPY_AND_ASSIGN(ScopedLock);
};



// ----------------------------------------------------------------------------
// OS
//
// This class has static methods for the different platform specific
// functions. Add methods here to cope with differences between the
// supported platforms.

class OS {
 public:
  // Initializes the platform OS support. Called once at VM startup.
  static void Setup();

  // Returns the accumulated user time for thread. This routine
  // can be used for profiling. The implementation should
  // strive for high-precision timer resolution, preferable
  // micro-second resolution.
  static int GetUserTime(uint32_t* secs,  uint32_t* usecs);

  // Get a tick counter normalized to one tick per microsecond.
  // Used for calculating time intervals.
  static int64_t Ticks();

  // Returns current time as the number of milliseconds since
  // 00:00:00 UTC, January 1, 1970.
  static double TimeCurrentMillis();

  // Returns a string identifying the current time zone. The
  // timestamp is used for determining if DST is in effect.
  static const char* LocalTimezone(double time);

  // Returns the local time offset in milliseconds east of UTC without
  // taking daylight savings time into account.
  static double LocalTimeOffset();

  // Returns the daylight savings offset for the given time.
  static double DaylightSavingsOffset(double time);

  // Returns last OS error.
  static int GetLastError();

  static FILE* FOpen(const char* path, const char* mode);
  static bool Remove(const char* path);

  // Opens a temporary file, the file is auto removed on close.
  static FILE* OpenTemporaryFile();

  // Log file open mode is platform-dependent due to line ends issues.
  static const char* const LogFileOpenMode;

  // Print output to console. This is mostly used for debugging output.
  // On platforms that has standard terminal output, the output
  // should go to stdout.
  static void Print(const char* format, ...);
  static void VPrint(const char* format, va_list args);

  // Print output to a file. This is mostly used for debugging output.
  static void FPrint(FILE* out, const char* format, ...);
  static void VFPrint(FILE* out, const char* format, va_list args);

  // Print error output to console. This is mostly used for error message
  // output. On platforms that has standard terminal output, the output
  // should go to stderr.
  static void PrintError(const char* format, ...);
  static void VPrintError(const char* format, va_list args);

  // Allocate/Free memory used by JS heap. Pages are readable/writable, but
  // they are not guaranteed to be executable unless 'executable' is true.
  // Returns the address of allocated memory, or NULL if failed.
  static void* Allocate(const size_t requested,
                        size_t* allocated,
                        bool is_executable);
  static void Free(void* address, const size_t size);

  // Mark code segments non-writable.
  static void ProtectCode(void* address, const size_t size);

  // Assign memory as a guard page so that access will cause an exception.
  static void Guard(void* address, const size_t size);

  // Generate a random address to be used for hinting mmap().
  static void* GetRandomMmapAddr();

  // Get the Alignment guaranteed by Allocate().
  static size_t AllocateAlignment();

  // Returns an indication of whether a pointer is in a space that
  // has been allocated by Allocate().  This method may conservatively
  // always return false, but giving more accurate information may
  // improve the robustness of the stack dump code in the presence of
  // heap corruption.
  static bool IsOutsideAllocatedSpace(void* pointer);

  // Sleep for a number of milliseconds.
  static void Sleep(const int milliseconds);

  // Abort the current process.
  static void Abort();

  // Debug break.
  static void DebugBreak();

  // Walk the stack.
  static const int kStackWalkError = -1;
  static const int kStackWalkMaxNameLen = 256;
  static const int kStackWalkMaxTextLen = 256;
  struct StackFrame {
    void* address;
    char text[kStackWalkMaxTextLen];
  };

  // Factory method for creating platform dependent Mutex.
  // Please use delete to reclaim the storage for the returned Mutex.
  static Mutex* CreateMutex();

  class MemoryMappedFile {
   public:
    static MemoryMappedFile* open(const char* name);
    static MemoryMappedFile* create(const char* name, int size, void* initial);
    virtual ~MemoryMappedFile() { }
    virtual void* memory() = 0;
    virtual int size() = 0;
  };

  // Support for the profiler.  Can do nothing, in which case ticks
  // occuring in shared libraries will not be properly accounted for.
  static void LogSharedLibraryAddresses();

  // Support for the profiler.  Notifies the external profiling
  // process that a code moving garbage collection starts.  Can do
  // nothing, in which case the code objects must not move (e.g., by
  // using --never-compact) if accurate profiling is desired.
  static void SignalCodeMovingGC();

  // The return value indicates the CPU features we are sure of because of the
  // OS.  For example MacOSX doesn't run on any x86 CPUs that don't have SSE2
  // instructions.
  // This is a little messy because the interpretation is subject to the cross
  // of the CPU and the OS.  The bits in the answer correspond to the bit
  // positions indicated by the members of the CpuFeature enum from globals.h
  static uint64_t CpuFeaturesImpliedByPlatform();

  // Maximum size of the virtual memory.  0 means there is no artificial
  // limit.
  static intptr_t MaxVirtualMemory();

  // Returns the double constant NAN
  static double nan_value();

  // Support runtime detection of whether the hard float option of the
  // EABI is used.
  static bool ArmUsingHardFloat();

  // Returns the activation frame alignment constraint or zero if
  // the platform doesn't care. Guaranteed to be a power of two.
  static int ActivationFrameAlignment();

#if defined(V8_TARGET_ARCH_IA32)
  // Copy memory area to disjoint memory area.
  static void MemCopy(void* dest, const void* src, size_t size);
  // Limit below which the extra overhead of the MemCopy function is likely
  // to outweigh the benefits of faster copying.
  static const int kMinComplexMemCopy = 64;
  typedef void (*MemCopyFunction)(void* dest, const void* src, size_t size);

#else  // V8_TARGET_ARCH_IA32
  static void MemCopy(void* dest, const void* src, size_t size) {
    memcpy(dest, src, size);
  }
  static const int kMinComplexMemCopy = 256;
#endif  // V8_TARGET_ARCH_IA32

 private:
  static const int msPerSecond = 1000;

};




// ----------------------------------------------------------------------------
// Thread
//
// Thread objects are used for creating and running threads. When the start()
// method is called the new thread starts running the run() method in the new
// thread. The Thread object should not be deallocated before the thread has
// terminated.

class Thread {
 public:
  // Opaque data type for thread-local storage keys.
  // LOCAL_STORAGE_KEY_MIN_VALUE and LOCAL_STORAGE_KEY_MAX_VALUE are specified
  // to ensure that enumeration type has correct value range (see Issue 830 for
  // more details).
  enum LocalStorageKey {
    LOCAL_STORAGE_KEY_MIN_VALUE = kMinInt,
    LOCAL_STORAGE_KEY_MAX_VALUE = kMaxInt
  };

  struct Options {
    Options() : name("v8:<unknown>"), stack_size(0) {}

    const char* name;
    int stack_size;
  };

  // Create new thread.
  explicit Thread(const Options& options);
  explicit Thread(const char* name);
  virtual ~Thread();

  // Start new thread by calling the Run() method in the new thread.
  void Start();

  // Wait until thread terminates.
  void Join();

  inline const char* name() const {
    return name_;
  }

  // Abstract method for run handler.
  virtual void Run() = 0;

  // Thread-local storage.
  static LocalStorageKey CreateThreadLocalKey();
  static void DeleteThreadLocalKey(LocalStorageKey key);
  static void* GetThreadLocal(LocalStorageKey key);
  static int GetThreadLocalInt(LocalStorageKey key) {
    return static_cast<int>(reinterpret_cast<intptr_t>(GetThreadLocal(key)));
  }
  static void SetThreadLocal(LocalStorageKey key, void* value);
  static void SetThreadLocalInt(LocalStorageKey key, int value) {
    SetThreadLocal(key, reinterpret_cast<void*>(static_cast<intptr_t>(value)));
  }
  static bool HasThreadLocal(LocalStorageKey key) {
    return GetThreadLocal(key) != NULL;
  }

#ifdef V8_FAST_TLS_SUPPORTED
  static inline void* GetExistingThreadLocal(LocalStorageKey key) {
    void* result = reinterpret_cast<void*>(
        InternalGetExistingThreadLocal(static_cast<intptr_t>(key)));
    ASSERT(result == GetThreadLocal(key));
    return result;
  }
#else
  static inline void* GetExistingThreadLocal(LocalStorageKey key) {
    return GetThreadLocal(key);
  }
#endif

  // A hint to the scheduler to let another thread run.
  static void YieldCPU();


  // The thread name length is limited to 16 based on Linux's implementation of
  // prctl().
  static const int kMaxThreadNameLength = 16;

  class PlatformData;
  PlatformData* data() { return data_; }

 private:
  void set_name(const char *name);

  PlatformData* data_;

  char name_[kMaxThreadNameLength];
  int stack_size_;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};



// ----------------------------------------------------------------------------
// Sampler
//
// A sampler periodically samples the state of the VM and optionally
// (if used for profiling) the program counter and stack pointer for
// the thread that created it.

// TickSample captures the information collected for each sample.
class TickSample {
 public:
  TickSample()
      :
        pc(NULL),
        sp(NULL),
        fp(NULL),
        function(NULL),
        frames_count(0) {}
  Address pc;  // Instruction pointer.
  Address sp;  // Stack pointer.
  Address fp;  // Frame pointer.
  Address function;  // The last called JS function.
  static const int kMaxFramesCount = 64;
  Address stack[kMaxFramesCount];  // Call stack.
  int frames_count;  // Number of captured frames.
};

class Sampler {
 public:
  // Initialize sampler.
  explicit Sampler(int interval, bool profiling);
  virtual ~Sampler();

  int interval() const { return interval_; }

  // Performs stack sampling.
  virtual void SampleStack(TickSample* sample) = 0;

  // This method is called for each sampling period with the current
  // program counter.
  virtual void Tick(TickSample* sample) = 0;

  // Request a save from a signal handler
  virtual void RequestSave() = 0;
  // Process any outstanding request outside a signal handler.
  virtual void HandleSaveRequest() = 0;

  // Start and stop sampler.
  void Start();
  void Stop();

  // Is the sampler used for profiling?
  bool IsProfiling() const { return profiling_; }

  // Whether the sampler is running (that is, consumes resources).
  bool IsActive() const { return active_; }

  class PlatformData;

  PlatformData* platform_data() { return data_; }

 private:
  void SetActive(bool value) { NoBarrier_Store(&active_, value); }

  const int interval_;
  const bool profiling_;
  Atomic32 active_;
  PlatformData* data_;  // Platform specific data.
};

