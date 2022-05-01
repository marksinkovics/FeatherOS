set(CMAKE_SYSTEM_NAME           Generic)
set(CMAKE_SYSTEM_PROCESSOR      x86_64)

# Without that flag CMake is not able to pass test compilation check
set(CMAKE_TRY_COMPILE_TARGET_TYPE   STATIC_LIBRARY)

set(CMAKE_AR                    x86_64-elf-ar${CMAKE_EXECUTABLE_SUFFIX})
set(CMAKE_ASM_COMPILER          x86_64-elf-gcc${CMAKE_EXECUTABLE_SUFFIX})
set(CMAKE_C_COMPILER            x86_64-elf-gcc${CMAKE_EXECUTABLE_SUFFIX})
set(CMAKE_CXX_COMPILER          x86_64-elf-g++${CMAKE_EXECUTABLE_SUFFIX})
set(CMAKE_LINKER                x86_64-elf-ld${CMAKE_EXECUTABLE_SUFFIX})
set(CMAKE_OBJCOPY               x86_64-elf-objcopy${CMAKE_EXECUTABLE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_RANLIB                x86_64-elf-ranlib${CMAKE_EXECUTABLE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_SIZE                  x86_64-elf-size${CMAKE_EXECUTABLE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_STRIP                 x86_64-elf-strip${CMAKE_EXECUTABLE_SUFFIX} CACHE INTERNAL "")

SET(COMMON_LDFLAGS "-T ${PROJECT_SOURCE_DIR}/src/linker.ld -nostdlib -zmax-page-size=0x1000 -static")

SET(CMAKE_EXE_LINKER_FLAGS      "${COMMON_LDFLAGS} --sysroot=${CMAKE_FIND_ROOT_PATH}")
SET(CMAKE_SHARED_LINKER_FLAGS   "--sysroot=${CMAKE_FIND_ROOT_PATH}")
SET(CMAKE_MODULE_LINKER_FLAGS   "--sysroot=${CMAKE_FIND_ROOT_PATH}")

set(COMMON_FLAGS               "-g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -Wextra -O0 -Iinc -fno-stack-protector -fno-stack-check -fno-pic -fno-pie -mcmodel=kernel")
set(COMMON_FLAGS_DEBUG         "-Os -g")
set(COMMON_FLAGS_RELEASE       "-Os -DNDEBUG")

set(CMAKE_C_FLAGS               "${COMMON_FLAGS} -std=gnu11" CACHE INTERNAL "")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_DEBUG         "${COMMON_FLAGS_DEBUG}" CACHE INTERNAL "")
set(CMAKE_C_FLAGS_RELEASE       "${COMMON_FLAGS_RELEASE}" CACHE INTERNAL "")

set(CMAKE_CXX_FLAGS             "${COMMON_FLAGS} -fno-exceptions -fno-rtti" CACHE INTERNAL "")
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS_DEBUG       "${COMMON_FLAGS_DEBUG}" CACHE INTERNAL "")
set(CMAKE_CXX_FLAGS_RELEASE     "${COMMON_FLAGS_RELEASE}" CACHE INTERNAL "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
