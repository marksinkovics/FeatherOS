import FeatherLibC

// Swift does not import C's variadic functions, so these give open, openat, fcntl and ioctl the
// fixed parameters Glibc's Swift overlay gives them.

public func open(_ path: UnsafePointer<CChar>, _ flags: Int32) -> Int32 {
    featheros_open(path, flags, 0)
}

public func open(_ path: UnsafePointer<CChar>, _ flags: Int32, _ mode: mode_t) -> Int32 {
    featheros_open(path, flags, mode)
}

public func openat(_ directory: Int32, _ path: UnsafePointer<CChar>, _ flags: Int32) -> Int32 {
    featheros_openat(directory, path, flags, 0)
}

public func openat(
    _ directory: Int32, _ path: UnsafePointer<CChar>, _ flags: Int32, _ mode: mode_t
) -> Int32 {
    featheros_openat(directory, path, flags, mode)
}

public func fcntl(_ descriptor: Int32, _ command: Int32) -> Int32 {
    featheros_fcntl(descriptor, command, 0)
}

public func fcntl(_ descriptor: Int32, _ command: Int32, _ value: Int32) -> Int32 {
    featheros_fcntl(descriptor, command, Int(value))
}

public func fcntl(
    _ descriptor: Int32, _ command: Int32, _ pointer: UnsafeMutableRawPointer
) -> Int32 {
    featheros_fcntl(descriptor, command, Int(bitPattern: pointer))
}

public func ioctl(_ descriptor: Int32, _ request: UInt) -> Int32 {
    featheros_ioctl(descriptor, request, nil)
}

public func ioctl(_ descriptor: Int32, _ request: UInt, _ value: Int32) -> Int32 {
    featheros_ioctl(descriptor, request, UnsafeMutableRawPointer(bitPattern: Int(value)))
}

public func ioctl(
    _ descriptor: Int32, _ request: UInt, _ pointer: UnsafeMutableRawPointer
) -> Int32 {
    featheros_ioctl(descriptor, request, pointer)
}
