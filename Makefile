agent: jni/main.cc jni/wire.pb.cc
	ndk-build

jni/wire.pb.cc: wire.proto
	protoc --cpp_out=jni $< 

install: agent
ifndef STF_VENDOR 
	$(error STF_VENDOR is undefined)
else
	cp libs/armeabi-v7a/kaiosagent $$STF_VENDOR/STFService/kaiosagent 
endif
 
clean:
	rm -rf libs/ obj/
