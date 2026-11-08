ARCH := -march=armv6k -mtp=soft -mtpcs-leaf-frame -mfloat-abi=hard -mtune=mpcore -mfpu=vfp
COMMONFLAGS := $(ARCH) -O2 -fno-math-errno
CXXFLAGS := $(COMMONFLAGS)
