# Running the Video Compressor 
## 1. Open the project



## 2. Open the VS Code terminal

Use **Terminal → New Terminal**, or press:

```text
Control + `
```

Confirm that the terminal is in the project directory:

```sh
pwd
```

The result should be:

```text
/Users/aarohigattewar/OSPro
```

If it is not, run:

```sh
cd /Users/aarohigattewar/OSPro
```

## 3. Install the required tools

Install Apple's command-line compiler tools if they are not already installed:

```sh
xcode-select --install
```

Install FFmpeg using Homebrew:

```sh
brew install ffmpeg
```

These installation commands normally need to be run only once.

Verify the tools:

```sh
cc --version
make --version
ffmpeg -version
```

## 4. Build the project

```sh
make clean
make
```

This creates the following programs:

```text
video_compressor
video_decoder
```

## 5. Run the tests

```sh
make test
```

The command should finish without errors and print the DCT, prediction,
wavefront, thread-pool, round-trip, and PSNR test results.

## 6. Add a video

Copy a video such as `sample.mp4` into the `OSPro` folder. Alternatively, use
the video's complete path when running the demo.

## 7. Run the automated demo

Give the script execution permission:

```sh
chmod +x demo.sh
```

Run the default five-second demo:

```sh
./demo.sh sample.mp4
```

To choose the duration and output directory:

```sh
./demo.sh sample.mp4 5 demo_output
```

The script will:

1. Convert the input video to YUV420.
2. Run the encoder with four pthread workers.
3. Print the encoder wall-clock time and compression ratio.
4. Decode the compressed bitstream.
5. Create a viewable MP4.
6. Open the decoded preview.

The generated files are placed in `demo_output`:

```text
input.yuv
compressed.bin
decoded.yuv
decoded-preview.mp4
encoder-time.txt
```

If the preview does not open automatically, run:

```sh
open demo_output/decoded-preview.mp4
```

## Complete command flow

After opening the project in VS Code, this is the main sequence to copy and
paste into the VS Code terminal:

```sh
cd /Users/aarohigattewar/OSPro
brew install ffmpeg
make clean
make
make test
chmod +x demo.sh
./demo.sh sample.mp4 5 demo_output
```

Replace `sample.mp4` with the name or full path of your video.

## Common problems

### `brew: command not found`

Install Homebrew from [brew.sh](https://brew.sh), reopen the VS Code terminal,
and run:

```sh
brew install ffmpeg
```

### `make: command not found` or no C compiler

Install Apple's command-line tools:

```sh
xcode-select --install
```

### `Permission denied: ./demo.sh`

```sh
chmod +x demo.sh
```

### `video not found`

Check the filename:

```sh
ls
```

Or provide the complete path:

```sh
./demo.sh /Users/your-name/Movies/sample.mp4 5 demo_output
```
