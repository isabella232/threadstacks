cc_library(
    name = "signal_handler",
    srcs = ["signal_handler.cc"],
    hdrs = ["signal_handler.h"],
    deps = ["//common:channel",
            "//common:defer",
            "//common:sysutil",
            "//common:types",
            ":stack_tracer",
            "@com_google_absl//absl/debugging:symbolize",
            "@com_github_google_glog//:glog", ],
    linkopts = ["-lunwind"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "stack_tracer",
    srcs = ["stack_tracer.cc"],
    hdrs = ["stack_tracer.h"],
    deps = [
        "@com_google_absl//absl/debugging:symbolize",
        "@com_github_google_glog//:glog",
    ],
    linkopts = ["-lunwind"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "signal_handler_test",
    srcs = ["signal_handler_test.cc"],
    deps = [":signal_handler",
            "//common:sysutil",
            "//external:gtest"],
    linkstatic = 1,
)
