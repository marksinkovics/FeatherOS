import Swift

// As on Linux, the default executors are upstream's Dispatch ones; on FeatherOS their
// swift_dispatch* functions run jobs on ../swift_concurrency/global_executor.cpp's threads.
@_spi(ExperimentalCustomExecutors)
@available(StdlibDeploymentTarget 6.3, *)
public struct PlatformExecutorFactory: ExecutorFactory {
    public static let mainExecutor: any MainExecutor = DispatchMainExecutor()
    public static let defaultExecutor: any TaskExecutor = DispatchGlobalTaskExecutor()
}
