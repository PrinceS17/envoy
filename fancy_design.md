

## Flexible Granularity Log Control in Envoy - Draft Design
Jinhui Song
June 25, 2020 [Updated on July 27]

### Abstract
We develop a new logging system Fancy Logger for Envoy which enables log level control of a more flexible granularity, e.g. file, function, or even line, as well as runtime level control without requiring developers to explicit use `Loggable` component in each source file.

### Background
<!-- problem of envoy's logger-->
The logger that Envoy takes use of is from a fast C++ logging library [spdlog](https://github.com/gabime/spdlog), which is fast, thread-safe, easy to format and has many other good features. Envoy uses class `Registry` to register all loggers, and template class `Loggable` is defined to store and return the logger for each module as follows:
 ```
 template <Id id> class Loggable {
protected:
  /**
   * Do not use this directly, use macros defined below.
   * @return spdlog::logger& the static log instance to use for class local logging.
   */
  static spdlog::logger& __log_do_not_use_read_comment() {
    static spdlog::logger& instance = Registry::getLog(id);
    return instance;
  }
};
```

Each module that wants logging must inherit from `Loggable` because the log macros that are commonly used like `ENVOY_LOG` use `Loggable<Id>::__log_do_not_use_read_comment()` to get logger:
```
#define ENVOY_LOGGER() __log_do_not_use_read_comment()
...
#define ENVOY_LOG(LEVEL, ...) ENVOY_LOG_TO_LOGGER(ENVOY_LOGGER(), LEVEL, ##__VA_ARGS__)
...
```

Obviously, this organization of the loggers in Envoy is suboptimal for the following reasons: 
1. The list of modules that can have logger is hard-coded;
2. Each logging module is required to inherit from `Loggable` explicitly in source file.
3. The granularity of module is not satisfying;

**First**, current Envoy logging system hard-codes a list of module IDs for a global logger registration like this:
```
#define ALL_LOGGER_IDS(FUNCTION)                                                                   \
  FUNCTION(admin)                                                                                  \
  FUNCTION(aws)                                                                                    \
  FUNCTION(assert)                                                                                 \
  ...                                         
  ```
  
  Only the modules defined here have the ID to specialize a `Loggable` class. This is not ideal because there are still needs for extensions to implement logging which are not belong to any existing module. 

**Second**, the implementation in each module is redundant and should be avoided as many other logging systems like [Google3 log](https://github.com/google/glog). 

**Third**, the granularity of module is too coarse especially in Google's production environments, where Envoy may have thousands of QPS and the logs of a single module are too many to analyze for debugging if its logging is enabled. It would be much better if we can develop a fine grained logging system with a granularity of file or even smaller.

### Goals
Considering the weakness of Envoy's previous logging system, we design a new system with theses goals.
1. A smaller granularity for a finer grained control, e.g. file level log control, with a dynamic registration.
2. No extra implementation in the source file needed other than the log macro usage.
3. A migration path that provides options for both old and new logger usage.
4. Runtime log level control at the admin page.


### Design
#### Global Flat Hash Map
<!-- key, value pair, FANCY_KEY staff -->
We choose flat hash map as the global data structure to dynamically store *\<key-logger\>* pairs. Here the key is the identifier of the logging component, which can be file, function and line. The logger is stored by pointer. Though spdlog logger is thread-safe, the map itself needs lock to protect the change of map. As the pointer of a certain logger won't be changed when the logger level is updated, the only change of modifying the map is global logger initialization. Thus we use a write lock to protect it.

When the logger is not found in map given a key, `createLogger(key, level)` is called to initialize the logger globally. It creates the logger, sets log level and pattern, and adds the logger pointer to the global map. The pointer is returned to also initialize the local logger pointer, which is covered in the next section.

#### Local Macro `FANCY_LOG`
The log macro `FANCY_LOG(LEVEL, ...)` is the core design of Fancy Logger. 
```
#define FANCY_LOG(LEVEL, ...)                                                                      \
  do {                                                                                             \
    static std::atomic<spdlog::logger*> flogger{0};                                                \
    spdlog::logger* local_flogger = flogger.load(std::memory_order_relaxed);                       \
    if (!local_flogger) {                                                                          \
      getFancyContext().initFancyLogger(FANCY_KEY, flogger);                                       \
      local_flogger = flogger.load(std::memory_order_relaxed);                                     \
    }                                                                                              \
    local_flogger->log(spdlog::source_loc{__FILE__, __LINE__, __func__},                           \
                       ENVOY_SPDLOG_LEVEL(LEVEL), __VA_ARGS__);                                    \
  } while (0)
  ```
  
  Here, we use a static atomic logger pointer `flogger` which enables thread-safe initialization and usage. Only when flogger is `nullptr` will we initialize the logger, which guarantees the speed of our macro. The case that logger is initialized and macro directly logs is called *fast path*. 
  
When flogger is `nullptr`, there are two cases due to the N-to-1 mapping between macro call sites and logging component. If the global logger doesn't exist, then `createLogger(key, level)` is called to create one and store the pointer in `flogger`. This is called *slow path* as it's the slowest operation in Fancy Logger. Otherwise, the global logger is created by another site with the same key, so the global pointer is fetched at once and stored in `flogger`. This is called *medium path*.

#### Log Level Update
The following functions are provided to set log level of Fancy Logger: `bool setFancyLogger(key, level), void setAllFancyLoggers(level)`. The former updates the log level of a certain log component with given key and return false if not found, and the latter updates the log levels of all loggers. They are both used to hook admin page with the update of Fancy Logger.

#### Migration
<!-- give choices to developers -->
Aside from `FANCY_LOG`, `FANCY_CONN_LOG`, `FANCY_STREAM_LOG` and `FANCY_FLUSH_LOG` are also implemented to replace the Envoy's convenient macros `ENVOY_CONN_LOG`, `ENVOY_STREAM_LOG` and `ENVOY_FLUSH_LOG`. For a smooth migration, Envoy's logger is used by default and we provide a compile option for developer to use Fancy Logger. In other words, if macro `FANCY` is not defined, `ENVOY_LOG` keeps the same as before but `FANCY_LOG` can be explicitly called; otherwise `ENVOY_LOG` is expanded to `FANCY_LOG` and all predefined Envoy's loggers are deprecated. 

#### Configuration Interfaces: Command Line and Administration Interface
<!-- log-path; log-format; runtime update API, etc -->
Users can use command line to set default log level and format of Fancy Logger just as what they do for Envoy's logger. It's implemented by calling `setDefaultFancyLevelFormat(log_level_, log_format_)` when initializing or switching the logging context of Envoy. The logger of default level will be updated to the new default level, and the rest will keep the same level as they are customized before.

Envoy provides an administration interface ("admin page") to enable dynamic change of logger. The [admin page for logger](https://www.envoyproxy.io/docs/envoy/latest/operations/admin#post--logging) supports the following operations: list all active loggers, set log level of a specific logger, and set levels for all loggers. Correspondingly, three APIs are provided to support same operations of Fancy Logger: `listFancyLoggers(), setFancyLogger(key, level), setAllFancyLoggers(level)`, and used in log handler of admin page. Once the logging context has a Fancy mode, the handler uses theses APIs to update Fancy Logger instead updating Envoy's logger.

**Reminder**: if `FANCY` isn't defined, the logger mode of context won't be FANCY, which means the handlers of Fancy Logger won't be enabled.

### Alternative Design
<!-- linked list as global -> local level & epoch-->
The main design we refer to is [Google3 Log](https://github.com/google/glog) (glog). In glog, there are two types of logging system: `LOG()` and `VLOG()`. The former creates a new message instance for each call site, so it doesn't take use of spdlog logger and also hard to integrate in Envoy. The latter uses a macro `VLOG_IS_ON(verboselevel)` to determine whether the message should be logged, which is feasible in Envoy. 

`VLOG_IS_ON(verboselevel)` uses a static variable `vlocal` to store the local log level, and a linked list to store global level information. If `vlocal` is uninitialized or outdated, slow path of glog is triggered to update `vlocal` and initialize global entry if needed. Restriction of this design in Envoy include: logger storage that is not considered, slow linked list and the complexity introduced by *epoch*.

To determine whether the local level is outdated, we need to synchronize between local time and global time, i.e. local epoch and global epoch. The **idea** is: we increment global epoch once level is modified, and if local epoch is less than global epoch, the local call site knows it's outdated and update local information. One **concern** is: we need to compare the epochs every time the macro is called, so we don't want lock inside macro. A solution is we use a global `atomic_int` to record if *any global level* is modified. If so, lock is acquired and we do slower work like more accurate comparison and update.

The reasons we choose current design include:
1. Considering lock is needed anyway, flat hash map is faster than linked list;
2. It's logger pointer but not level that is used locally which doesn't need update like `vlocal`!

The second reason saves us from the complexity of epoch because the local pointer is never outdated after initialization. This comes from the advantage of spdlog logger as it's thread-safe and thus can be used directly by pointer.


### Discussion
<!-- path analysis, performance, future work: format setting in command line, log path -->
#### Performance
Considering the real cases where the logger can be used, we prioritize the performance of fast path (with or without printing) and relatively deprioritize performance of slow path and log level setting as slow path occurs only once per call site and log level is probably only changed at the beginning or debugging time.

Benchmark tests of slow path, medium path and fast path are conducted, and Fancy Logger is also compared with Envoy's logger. Fancy Logger has a slower fast path when there's no printing, but the CPU time is still small enough. When printing log messages in single and multiple thread cases, Fancy Logger is even faster than Envoy's logger. Besides, Fancy Logger is much slower than Envoy's logger in level setting due to the extra work needed here (lock, update global map, etc), but it should be okay as the level changing is rare in reality.
**![](https://lh6.googleusercontent.com/Q-POZ2adRXV3gD-uwSUNPmmbbzsgk0kkxupy32h3iVDCF877b2yKIm0Z2Dc3B2s0HgIukqL1McDsfOpvpx2wc7DdrBLCzAIcHy99G90VLeGk773tBUEpoQAoFwaViBWhM10j3wU0YA)**

### References
Here are the existing logging system we refered to:
- Google3 Log: [https://github.com/google/glog](https://github.com/google/glog)
- ns-3 LogComponent: [https://www.nsnam.org/doxygen/classns3_1_1_log_component.html](https://www.nsnam.org/doxygen/classns3_1_1_log_component.html)
- spdlog: [https://github.com/gabime/spdlog](https://github.com/gabime/spdlog)
- Envoy's Administration Interface: [https://www.envoyproxy.io/docs/envoy/latest/operations/admin#administration-interface](https://www.envoyproxy.io/docs/envoy/latest/operations/admin#administration-interface)