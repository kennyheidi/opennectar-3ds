# New Nintendo 3DS (ARM11 MPCore, ARMv6K, VFPv2 hard-float) via devkitARM + libctru.
# UNTESTED: written from the devkitPro layout; the sandbox it was authored in could not reach
# pkg.devkitpro.org. Requires DEVKITPRO in the environment (devkitPro pacman: 3ds-dev).
if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO is not set. Install devkitPro and `pacman -S 3ds-dev`.")
endif()
set(DEVKITPRO $ENV{DEVKITPRO})
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(N3DS TRUE)
# CMake's compiler check would otherwise link a full executable with -specs=3dsx.specs, whose crt0 needs
# initSystem/__system_argc from libctru (not linked during the check) -> "not able to compile a simple test program".
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER   ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc)
set(CMAKE_AR           ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc-ar CACHE FILEPATH "")
set(CMAKE_RANLIB       ${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc-ranlib CACHE FILEPATH "")
set(N3DS_ARCH "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")
set(CMAKE_C_FLAGS_INIT   "${N3DS_ARCH} -D__3DS__ -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${N3DS_ARCH} -D__3DS__ -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=3dsx.specs ${N3DS_ARCH} -Wl,--gc-sections")
set(CMAKE_FIND_ROOT_PATH ${DEVKITPRO}/libctru ${DEVKITPRO}/portlibs/3ds)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
include_directories(${DEVKITPRO}/libctru/include ${DEVKITPRO}/portlibs/3ds/include)
link_directories(${DEVKITPRO}/libctru/lib ${DEVKITPRO}/portlibs/3ds/lib)
