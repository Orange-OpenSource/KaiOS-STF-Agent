LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_CPPFLAGS += -fPIE
LOCAL_LDFLAGS += -fPIE -pie
LOCAL_CPPFLAGS := -std=c++11

LOCAL_MODULE := kaiosagent 

# To test compilation, generate a .pb.cc file and add it to local_src_files,
# this will make the linker complain about everything that's missing because
# we're building a shared library
LOCAL_SRC_FILES := wire.pb.cc \
                   main.cc

LOCAL_STATIC_LIBRARIES += libprotobuf
LOCAL_LDLIBS := -lz

include $(BUILD_EXECUTABLE)

# This assumes protobuf-android is checked out in a directory named
# protobuf-android
$(call import-add-path, $(LOCAL_PATH)/../protobuf-android)
$(call import-module,protobuf-android)
