CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -pthread
SYNC_GRANULARITY ?= 1
PREDICTION ?= 1
CPPFLAGS := -Isrc -DSYNC_GRANULARITY=$(SYNC_GRANULARITY) \
	-DENABLE_PREDICTION=$(PREDICTION)
LDFLAGS := -pthread
LDLIBS := -lm

TARGET := video_compressor
SOURCES := src/main.c src/codec.c src/threadpool.c src/reorder.c \
	src/wavefront.c
OBJECTS := $(SOURCES:.c=.o)
DECODER := video_decoder
DECODER_SOURCES := src/decode_main.c src/decode.c src/codec.c
TEST_TARGETS := test/test_read_frame test/test_dct test/test_prediction \
	test/test_reconstruct test/test_entropy test/generate_fixture test/psnr
TEST_TARGETS += test/test_wavefront
TEST_TARGETS += test/test_encode_row
TEST_TARGETS += test/test_threadpool
PIPELINE_INPUT := /tmp/ospro_phase1_fixture.yuv
PIPELINE_OUTPUT := /tmp/ospro_phase1_output.bin
PIPELINE_DECODED := /tmp/ospro_phase1_decoded.yuv

.PHONY: all clean test

all: $(TARGET) $(DECODER)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(DECODER): $(DECODER_SOURCES) src/decode.h src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DECODER_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test/test_read_frame: test/test_read_frame.c src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/test_dct: test/test_dct.c src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/test_prediction: test/test_prediction.c src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/test_reconstruct: test/test_reconstruct.c src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/test_entropy: test/test_entropy.c src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/generate_fixture: test/generate_fixture.c
	$(CC) $(CFLAGS) $< -o $@

test/psnr: test/psnr.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

test/test_wavefront: test/test_wavefront.c src/wavefront.c src/wavefront.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_wavefront.c src/wavefront.c \
		$(LDFLAGS) -o $@

test/test_encode_row: test/test_encode_row.c src/threadpool.c src/threadpool.h \
		src/wavefront.c src/wavefront.h src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_encode_row.c src/threadpool.c \
		src/wavefront.c src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test/test_threadpool: test/test_threadpool.c src/threadpool.c src/threadpool.h \
		src/wavefront.c src/wavefront.h src/codec.c src/codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_threadpool.c src/threadpool.c \
		src/wavefront.c src/codec.c $(LDFLAGS) $(LDLIBS) -o $@

test: $(TARGET) $(DECODER) $(TEST_TARGETS)
	./test/test_read_frame
	./test/test_dct
	./test/test_prediction
	./test/test_reconstruct
	./test/test_entropy
	./test/test_wavefront
	./test/test_encode_row
	./test/test_threadpool
	./test/generate_fixture $(PIPELINE_INPUT)
	./$(TARGET) $(PIPELINE_INPUT) $(PIPELINE_OUTPUT) 32 32
	./$(DECODER) $(PIPELINE_OUTPUT) $(PIPELINE_DECODED) 32 32
	./test/psnr $(PIPELINE_INPUT) $(PIPELINE_DECODED)
	@input_size=$$(wc -c < $(PIPELINE_INPUT)); \
	output_size=$$(wc -c < $(PIPELINE_OUTPUT)); \
	echo "pipeline sizes: input=$$input_size bytes output=$$output_size bytes"; \
	test $$output_size -lt $$input_size

clean:
	$(RM) $(OBJECTS) $(TARGET) $(DECODER) $(TEST_TARGETS)
