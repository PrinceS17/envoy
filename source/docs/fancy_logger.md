## Fancy Logger: Flexible Granularity Log Control in Envoy

### Overview
Fancy Logger is a logger with finer grained log level control and runtime logger update using administration interface. Compared to the existing logger in Envoy, Fancy Logger provides file level control which can be easily extended to finer grained logger with function or line level control, and it is completely automatic and never requires developers to explicitly specify the logging component. Besides, it has a comparable speed as Envoy's logger. 

### Basic Usage
The basic usage of Fancy Logger is to explicitly call its macros:
```
  FANCY_LOG(info, "Hello world! Here's a line of fancy log!");
  FANCY_LOG(error, "Fancy Error! Here's the second message!");
```
If the level of log message is higher than that of the file, macros above will print messages with the file name like this:
```
[2020-07-29 22:27:02.594][15][error][test/common/common/log_macros_test.cc] [test/common/common/log_macros_test.cc:149] Fancy Error! Here\'s the second message!
```
More macros with connection and stream information:
```
  NiceMock<Network::MockConnection> connection_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> stream_;
  FANCY_CONN_LOG(warn, "Fake info {} of connection", connection_, 1);
  FANCY_STREAM_LOG(warn, "Fake warning {} of stream", stream_, 1);
```
To flush a logger, `FANCY_FLUSH_LOG()` can be used. 

### Logger Mode: Fancy or Envoy
By default, Envoy's logger is used and a compile option is provided to enable Fancy Logger. If symbol `FANCY` is defined using `--copt` like the following,
```
bazel test -c opt --copt="-D FANCY" //test/common/common:log_macros_test
```
the mode is **Fancy mode**. Fancy Logger is enabled and most Envoy's log macros (`ENVOY_LOG, ENVOY_FLUSH_LOG, ENVOY_CONN_LOG, ENVOY_STREAM_LOG`) are expanded to corresponding macros of Fancy Logger. In this mode, the change is transparent and developers can continue to use `ENVOY_LOG`. If logging context is defined by a main program (which is the common case), the given default log format and log level will be passed to Fancy Logger; otherwise the default log level is `info` and default format string is `"[%Y-%m-%d %T.%e][%t][%l][%n] %v"`, which is the same as Envoy. 

Note that Envoy's logger can also be used in Fancy mode. These macros are not replaced: `GET_MISC_LOGGER, ENVOY_LOG_MISC, ENVOY_LOGGER, ENVOY_LOG_TO_LOGGER, ENVOY_CONN_LOG_TO_LOGGER, ENVOY_STREAM_LOG_TO_LOGGER`. For example, `ENVOY_LOG_LOGGER(ENVOY_LOGGER(), LEVEL, ...)` is equivalent to `ENVOY_LOG` in Envoy mode. 

If symbol `FANCY` is not defined, it's **Envoy mode** with existing Envoy's logger being used. In this mode, basic macros like `FANCY_LOG` can be used but the main part of `ENVOY_LOG` will keep the same. One limitation is that logger update in admin page is not supported by default as it detects Envoy mode. The reason is: Envoy mode is designed only to be back compatible. To address it, developers can use `Logger::Context::setLoggerMode(Logger::LoggerMode::Fancy)` to manually set logger mode to Fancy. Note that it enables the admin page support but **not the macro expansion** as it's a runtime setting.


### Runtime Update
Runtime update of Fancy Logger is supported with administration interface, i.e. admin page, and Fancy mode needs to be enabled to use it. Same as Envoy's logger, the following functionalities are provided:

1. `POST /logging`: List all active loggers and their levels;
2. `POST /logging?<file_path>=<level>`: Given path from Envoy's root directory, change the log level of the file;
3. `POST /logging?level=<level>`: Change levels of all loggers.

Users can view and change the log level in a granularity of file in runtime through admin page.

### Implementation Details
Fancy Logger can be divided into two parts: 
1. Core part: file level logger without explicit inheriting `Loggable`;
2. Hook part: control interfaces, such as command line and admin page. 

#### Core Part
The core of Fancy Logger is implemented in `class FancyContext`, which is a singleton class. Filenames (i.e. keys) and logger pointers are stored in `FancyContext::fancy_log_map_`. There are several code paths when `FANCY_LOG` is called.

1. Slow path: if `FANCY_LOG` is first called in a file, `FancyContext::initFancyLogger(key, local_logger_ptr)` is called to create a new logger globally and store the `<key, global_logger_ptr>` pair in `fancy_log_map_`. Then local logger pointer is updated and will never be changed. 
2. Medium path: if `FANCY_LOG` is first called at the call site but not in the file, `FancyContext::initFancyLogger(key, local_logger_ptr)` is also called but local logger pointer is quickly set to the global logger, as there is already global record of the given filename.
3. Fast path: if `FANCY_LOG` is called after first call at the site, the log is directly printed using local logger pointer.

#### Hook Part
Fancy Logger provides control interfaces through command line and admin page. 

To pass the arguments such as log format and default log level to Fancy Logger, `fancy_log_format` and `fancy_default_level` are added in `class Context` and they are updated when a new logging context is activated. `getFancyContext().setDefaultFancyLevelFormat(level, format)` is called in `Context::activate()` to set log format and update loggers' previous default level to the new level. 

To support the runtime update in admin page, log handler in admin page uses `getFancyContext().listFancyLoggers()` to show all Fancy Loggers, `getFancyContext().setFancyLogger(name, level)` to set a specific logger and `getFancyContext().setAllFancyLoggers(level)` to set all loggers.

