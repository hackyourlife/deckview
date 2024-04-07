#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GLFW/glfw3.h>
#include <signal.h>
#include <pthread.h>

#include <DeckLinkAPI.h>

#include "deckview.h"

#define	SCREEN_WIDTH	1920
#define	SCREEN_HEIGHT	1080

static GLFWwindow* window;
static int window_pos_x;
static int window_pos_y;
static bool is_fullscreen = false;

static IDeckLink* device = NULL;
static IDeckLinkInput* input = NULL;
static DeckLinkCaptureDelegate* delegate = NULL;

static IDeckLinkProfileAttributes* attributes = NULL;
static IDeckLinkDisplayMode* display_mode = NULL;

static GXRenderer renderer = { 0 };

static volatile BMDPixelFormat pixel_format = bmdFormat8BitYUV;
static const BMDVideoInputFlags input_flags = bmdVideoInputFlagDefault | bmdVideoInputEnableFormatDetection;
static const unsigned int sample_depth = 16;
static const unsigned int audio_channels = 2;

static unsigned int frame_depth = 8;
static unsigned int frame_width = 0;
static unsigned int frame_height = 0;
static void* frame = NULL;
static size_t frame_size = 0;

static float brightness = 1.0;
static bool clear = true;
static pthread_mutex_t mutex;

extern "C" {
extern const char yuv8_vert[];
extern const char yuv8_frag[];
extern const char yuv10_vert[];
extern const char yuv10_frag[];
}

static const float quad_vertices[] = {
	-1.0f, -1.0f,  0.0f,
	 1.0f, -1.0f,  0.0f,
	 1.0f,  1.0f,  0.0f,

	 1.0f,  1.0f,  0.0f,
	-1.0f,  1.0f,  0.0f,
	-1.0f, -1.0f,  0.0f
};

#define	QUAD_VTX_CNT	(sizeof(quad_vertices) / (sizeof(*quad_vertices) * 3))

static void error_handler(int error, const char* description)
{
	printf("Error 0x%X: %s\n", error, description);
}

static void sigfunc(int signum)
{
	if(signum == SIGINT || signum == SIGTERM) {
		glfwSetWindowShouldClose(window, GLFW_TRUE);
	}
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* mode, BMDDetectedVideoInputFormatFlags format_flags)
{
	// This only gets called if bmdVideoInputEnableFormatDetection was set

	BMDPixelFormat fmt = pixel_format;
	unsigned int depth = frame_depth;

	if(events & bmdVideoInputColorspaceChanged) {
		if(format_flags & bmdDetectedVideoInput8BitDepth) {
			depth = 8;
		} else if(format_flags & bmdDetectedVideoInput10BitDepth) {
			depth = 10;
		} else if(format_flags & bmdDetectedVideoInput12BitDepth) {
			depth = 12;
		}

		if(format_flags & bmdDetectedVideoInputRGB444) {
			fmt = bmdFormat10BitRGB;
		} else if(format_flags & bmdDetectedVideoInputYCbCr422) {
			switch(depth) {
				case 8:
					fmt = bmdFormat8BitYUV;
					break;
				case 10:
					fmt = bmdFormat10BitYUV;
					break;
				case 12:
					fmt = bmdFormat10BitYUV;
					break;
			}
		} else {
			goto bail;
		}
	}

	// Restart streams if either display mode or pixel format have changed
	if((events & bmdVideoInputDisplayModeChanged) || (pixel_format != fmt)) {
		const char* display_mode_name;
		mode->GetName(&display_mode_name);
		printf("Video format changed to %s %s %d bit\n", display_mode_name, format_flags & bmdDetectedVideoInputRGB444 ? "RGB" : "YUV", depth);
		if(display_mode_name) {
			free((void*) display_mode_name);
		}

		frame_width = mode->GetWidth();
		frame_height = mode->GetHeight();

		if(!is_fullscreen) {
			glfwSetWindowSize(window, frame_width, frame_height);
		}

		pthread_mutex_lock(&mutex);
		switch(fmt) {
			case bmdFormat8BitYUV:
				frame_size = (frame_width * 16 / 8) * frame_height;
				break;
			case bmdFormat10BitYUV:
				frame_size = ((frame_width + 47) / 48) * 128 * frame_height;
				break;
			case bmdFormat10BitRGB:
				frame_size = ((frame_width + 63) / 64) * 256 * frame_height;
				break;
			default:
				frame_size = 0;
				break;
		}

		// resize framebuffer
		if(frame) {
			frame = realloc(frame, frame_size);
		} else {
			frame = malloc(frame_size);
		}

		memset(frame, 0, frame_size);

		pixel_format = fmt;
		frame_depth = depth;
		pthread_mutex_unlock(&mutex);

		if(input) {
			input->StopStreams();

			if(input->EnableVideoInput(mode->GetDisplayMode(), fmt, input_flags) != S_OK) {
				fprintf(stderr, "Failed to switch video mode\n");
				goto bail;
			}

			input->StartStreams();
		}
	}

bail:
	return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* video_frame, IDeckLinkAudioInputPacket* audio_frame)
{
	if(video_frame) {
		if(video_frame->GetFlags() & bmdFrameHasNoInputSource) {
			// TODO: maybe blank the video output in this case?
			// printf("No input signal detected\n");
		} else {
			void* frame_bytes;
			video_frame->GetBytes(&frame_bytes);

			const size_t size = video_frame->GetRowBytes() * video_frame->GetHeight();
			if(frame) {
				if(frame_size < size) {
					printf("fail! %lu vs %lu\n", frame_size, size);
				} else {
					pthread_mutex_lock(&mutex);
					memcpy(frame, frame_bytes, size);
					pthread_mutex_unlock(&mutex);
				}
			}
		}
	}

	if(audio_frame) {
		void* frame_bytes;
		audio_frame->GetBytes(&frame_bytes);
		size_t size = audio_frame->GetSampleFrameCount() * audio_channels * (sample_depth / 8);
		AXPlay(frame_bytes, size);
	}

	return S_OK;
}

bool GXInitDeckLink(IDeckLink* device)
{
	int64_t duplex_mode;
	bool format_detection_supported;
	bool supported;

	if(device->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attributes) != S_OK) {
		fprintf(stderr, "Unable to get DeckLink attributes interface\n");
		return false;
	}

	// Check the DeckLink device is active
	if(attributes->GetInt(BMDDeckLinkDuplex, &duplex_mode) != S_OK) {
		fprintf(stderr, "The selected DeckLink device is inactive\n");
		return false;
	}

	if(duplex_mode == bmdDuplexInactive) {
		fprintf(stderr, "The selected DeckLink device is inactive\n");
		return false;
	}

	// Get the input (capture) interface of the DeckLink device
	if(device->QueryInterface(IID_IDeckLinkInput, (void**) &input) != S_OK) {
		fprintf(stderr, "The selected device does not have an input interface\n");
		return false;
	}

	// Check the card supports format detection
	if(attributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &format_detection_supported) != S_OK) {
		fprintf(stderr, "Format detection is not supported on this device\n");
		return false;
	}

	if(!format_detection_supported) {
		fprintf(stderr, "Format detection is not supported on this device\n");
		return false;
	}

	// For format detection mode, use 1080p30 as default mode to start with
	if(input->GetDisplayMode(bmdModeHD1080p30, &display_mode) != S_OK) {
		fprintf(stderr, "Unable to get display mode\n");
		return false;
	}

	if(display_mode == NULL) {
		fprintf(stderr, "Unable to get display mode\n");
		return false;
	}

	// Check display mode is supported with given options
	if(input->DoesSupportVideoMode(bmdVideoConnectionUnspecified, display_mode->GetDisplayMode(), pixel_format, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault, NULL, &supported) != S_OK) {
		fprintf(stderr, "The display mode is not supported with the selected pixel format\n");
		return false;
	}

	if(!supported) {
		fprintf(stderr, "The display mode is not supported with the selected pixel format\n");
		return false;
	}

	delegate = new DeckLinkCaptureDelegate();
	input->SetCallback(delegate);

	return true;
}

#ifdef NDEBUG
#define	GL_ERROR()
#else
#define	GL_ERROR()	check_error(__FILE__, __LINE__)

void check_error(const char* filename, unsigned int line)
{
	GLenum error = glGetError();
	switch(error) {
		case GL_NO_ERROR:
			break;
		case GL_INVALID_ENUM:
			printf("%s:%u: Error: GL_INVALID_ENUM\n", filename, line);
			break;
		case GL_INVALID_VALUE:
			printf("%s:%u: Error: GL_INVALID_VALUE\n", filename, line);
			break;
		case GL_INVALID_OPERATION:
			printf("%s:%u: Error: GL_INVALID_OPERATION\n", filename, line);
			break;
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			printf("%s:%u: Error: GL_INVALID_FRAMEBUFFER_OPERATION\n", filename, line);
			break;
		case GL_OUT_OF_MEMORY:
			printf("%s:%u: Error: GL_OUT_OF_MEMORY\n", filename, line);
			exit(1);
			break;
		case GL_STACK_UNDERFLOW:
			printf("%s:%u: Error: GL_STACK_UNDERFLOW\n", filename, line);
			break;
		case GL_STACK_OVERFLOW:
			printf("%s:%u: Error: GL_STACK_OVERFLOW\n", filename, line);
			break;
		default:
			printf("%s:%u: Unknown error 0x%X\n", filename, line, error);
	}
}
#endif

void GXRender(int width, int height)
{
	GXRenderer* self = &renderer;

	bool interpolate = width != (int) frame_width || height != (int) frame_height;

	switch(pixel_format) {
		case bmdFormat8BitYUV:
			glUseProgram(self->yuv8_shader);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, self->frame);
			glUniform1i(self->yuv8_shader_tex, 0);
			glUniform1f(self->yuv8_shader_brightness, brightness);
			glUniform1f(self->yuv8_shader_interpolate, interpolate);

			pthread_mutex_lock(&mutex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_width / 2, frame_height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (GLvoid*) frame);
			pthread_mutex_unlock(&mutex);
			break;

		case bmdFormat10BitYUV:
			glUseProgram(self->yuv10_shader);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, self->frame);
			glUniform1i(self->yuv10_shader_tex, 0);
			glUniform2i(self->yuv10_shader_size, frame_width, frame_height);
			glUniform1f(self->yuv10_shader_brightness, brightness);
			glUniform1f(self->yuv10_shader_interpolate, interpolate);

			pthread_mutex_lock(&mutex);
			int tex_width = ((frame_width + 47) / 48) * 128 / 4;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, tex_width, frame_height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, (GLvoid*) frame);
			pthread_mutex_unlock(&mutex);
			GL_ERROR();
			break;
	}

	glBindVertexArray(self->quad_vao);
	glDrawArrays(GL_TRIANGLES, 0, QUAD_VTX_CNT);
}

static bool get_monitor(GLFWmonitor** monitor, GLFWwindow* window)
{
	int window_x;
	int window_y;

	glfwGetWindowPos(window, &window_x, &window_y);

	int window_w;
	int window_h;

	glfwGetWindowSize(window, &window_w, &window_h);

	int monitor_count;
	GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);

	int window_x1 = window_x + window_w;
	int window_y1 = window_y + window_h;

	int max_overlap = 0;
	GLFWmonitor* closest = NULL;

	for(int i = 0; i < monitor_count; i++) {
		GLFWmonitor* mon = monitors[i];

		int pos_x;
		int pos_y;

		glfwGetMonitorPos(mon, &pos_x, &pos_y);

		const GLFWvidmode* mode = glfwGetVideoMode(mon);

		int pos_x1 = pos_x + mode->width;
		int pos_y1 = pos_y + mode->height;

		/* overlap? */
		if(!(
			(window_x1 < pos_x) ||
			(window_x > pos_x1) ||
			(window_y < pos_y) ||
			(window_y > pos_y1)
		)) {
			int overlap_w = 0;
			int overlap_h = 0;

			/* x, width */
			if(window_x < pos_x) {
				if(window_x1 < pos_x1) {
					overlap_w = window_x1 - pos_x;
				} else {
					overlap_w = mode->width;
				}
			} else {
				if(pos_x1 < window_x1) {
					overlap_w = pos_x1 - window_x;
				} else {
					overlap_w = window_w;
				}
			}

			// y, height
			if(window_y < pos_y) {
				if(window_y1 < pos_y1) {
					overlap_h = window_y1 - pos_y;
				} else {
					overlap_h = mode->height;
				}
			} else {
				if(pos_y1 < window_y1) {
					overlap_h = pos_y1 - window_y;
				} else {
					overlap_h = window_h;
				}
			}

			int overlap = overlap_w * overlap_h;
			if(overlap > max_overlap) {
				closest = monitors[i];
				max_overlap = overlap;
			}
		}
	}

	if(closest) {
		*monitor = closest;
		return true;
	} else {
		return false;
	}
}

static void enter_fullscreen(void)
{
	glfwGetWindowPos(window, &window_pos_x, &window_pos_y);

	GLFWmonitor* mon;
	if(get_monitor(&mon, window)) {
		int pos_x;
		int pos_y;

		glfwGetMonitorPos(mon, &pos_x, &pos_y);
		const GLFWvidmode* mode = glfwGetVideoMode(mon);

		glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
		glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);

		glfwSetWindowPos(window, pos_x, pos_y);
		glfwSetWindowSize(window, mode->width, mode->height);

		is_fullscreen = true;
	}
}

static void exit_fullscreen(void)
{
	glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
	glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_FALSE);

	if(frame_width > 0 && frame_height > 0) {
		glfwSetWindowSize(window, frame_width, frame_height);
	} else {
		glfwSetWindowSize(window, SCREEN_WIDTH, SCREEN_HEIGHT);
	}
	glfwSetWindowPos(window, window_pos_x, window_pos_y);

	is_fullscreen = false;
}

static void toggle_fullscreen(void)
{
	if(is_fullscreen) {
		exit_fullscreen();
	} else {
		enter_fullscreen();
	}
}

static void key_handler(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if(action == GLFW_PRESS) {
		switch(key) {
			case GLFW_KEY_F2:
				toggle_fullscreen();
				break;
			case GLFW_KEY_F5:
				glfwSetWindowShouldClose(window, GLFW_TRUE);
				break;
			case GLFW_KEY_KP_ADD:
				brightness += 0.25;
				break;
			case GLFW_KEY_KP_SUBTRACT:
				brightness -= 0.25;
				break;
			case GLFW_KEY_KP_ENTER:
				brightness = 1.0;
				break;
			case GLFW_KEY_C:
				clear = !clear;
				break;
		}
	}
}

bool GXInit(IDeckLink* dev)
{
	device = dev;

	pthread_mutex_init(&mutex, NULL);

	if(!AXInit(audio_channels, sample_depth)) {
		printf("Failed to initialize audio\n");
		return false;
	}

	if(!GXInitDeckLink(device)) {
		return false;
	}

	glfwSetErrorCallback(error_handler);
	if(!glfwInit()) {
		printf("Failed to initialize GLFW\n");
		return false;
	}

	glfwWindowHint(GLFW_AUTO_ICONIFY, 0);

	window = glfwCreateWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "DeckLink View", NULL, NULL);
	if(!window) {
		printf("Failed to create GLFW window\n");
		glfwTerminate();
		return false;
	}

	signal(SIGINT, sigfunc);
	signal(SIGTERM, sigfunc);

	glfwSetKeyCallback(window, key_handler);

	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	const unsigned char* gl_vendor = glGetString(GL_VENDOR);
	const unsigned char* gl_renderer = glGetString(GL_RENDERER);
	const unsigned char* gl_version = glGetString(GL_VERSION);
	const unsigned char* gl_glsl_version = glGetString(GL_SHADING_LANGUAGE_VERSION);

	printf("GL Vendor:    %s\n", gl_vendor);
	printf("GL Renderer:  %s\n", gl_renderer);
	printf("GL Version:   %s\n", gl_version);
	printf("GLSL Version: %s\n", gl_glsl_version);

	glClearColor(0.0, 0.0, 0.0, 0.0);

	GXRendererInit(&renderer);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	return true;
}

void GXMain(void)
{
	if(input) {
		if(input->EnableVideoInput(display_mode->GetDisplayMode(), pixel_format, input_flags) != S_OK) {
			fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}

		if(input->EnableAudioInput(bmdAudioSampleRate48kHz, sample_depth, audio_channels) != S_OK) {
			fprintf(stderr, "Failed to enable audio input. Is another application using the card?\n");
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}

		if(input->StartStreams() != S_OK) {
			fprintf(stderr, "Failed to start streams\n");
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
	}

	AXStart();

	while(!glfwWindowShouldClose(window)) {
		int width;
		int height;

		glfwGetFramebufferSize(window, &width, &height);

		glViewport(0, 0, width, height);
		if(clear) {
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDisable(GL_BLEND);
		} else {
			glEnable(GL_BLEND);
		}

		GXRender(width, height);

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	AXStop();

	if(input) {
		input->StopStreams();
		input->DisableAudioInput();
		input->DisableVideoInput();
	}
}

void GXDestroy(void)
{
	if(display_mode) {
		display_mode->Release();
	}

	if(delegate) {
		delegate->Release();
	}

	if(input) {
		input->Release();
	}

	if(attributes) {
		attributes->Release();
	}

	if(frame) {
		free(frame);
	}

	AXStop();
	AXDestroy();

	glfwDestroyWindow(window);
	glfwTerminate();
}

////////////////////////////////////////////////////////////////////////////////
GLuint GXCompileShader(GLuint type, const char* src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, 0);
	glCompileShader(shader);

	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(compiled == GL_FALSE) {
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

		if(!len) {
			printf("Failed to retrieve shader compilation error log\n");
			exit(1);
		}

		char* log = (char*) malloc(len);
		glGetShaderInfoLog(shader, len, &len, log);
		glDeleteShader(shader);

		printf("Failed to compile shader:\n%s\n", log);
		free(log);

		exit(1);
	} else {
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

		if(len) {
			char* log = (char*) malloc(len);
			glGetShaderInfoLog(shader, len, &len, log);
			if(len) {
				printf("shader compilation log:\n%s\n", log);
			}
			free(log);
		}
	}

	return shader;
}

GLuint GXCreateShader(const char* vs_src, const char* fs_src)
{
	GLuint vs = GXCompileShader(GL_VERTEX_SHADER, vs_src);
	GLuint fs = GXCompileShader(GL_FRAGMENT_SHADER, fs_src);

	GLuint shader = glCreateProgram();

	glAttachShader(shader, vs);
	glAttachShader(shader, fs);
	glLinkProgram(shader);

	GLint linked = 0;
	glGetProgramiv(shader, GL_LINK_STATUS, &linked);
	if(linked == GL_FALSE) {
		GLint len = 0;
		glGetProgramiv(shader, GL_INFO_LOG_LENGTH, &len);

		if(!len) {
			printf("Faild to retrieve shader linking error log\n");
			exit(1);
		}

		char* log = (char*) malloc(len);
		glGetProgramInfoLog(shader, len, &len, log);

		glDeleteProgram(shader);
		glDeleteShader(vs);
		glDeleteShader(fs);

		printf("Failed to link shader:\n%s\n", log);

		free(log);
		exit(1);
	} else {
		GLint len = 0;
		glGetProgramiv(shader, GL_INFO_LOG_LENGTH, &len);

		if(len) {
			char* log = (char*) malloc(len);
			glGetProgramInfoLog(shader, len, &len, log);

			if(len) {
				printf("Shader linking error log:\n%s\n", log);
			}

			free(log);
		}
	}

	glDetachShader(shader, vs);
	glDetachShader(shader, fs);

	glDeleteShader(vs);
	glDeleteShader(fs);

	return shader;
}

void GXRendererInit(GXRenderer* self)
{
	GXCreateBuffers(self);
	GXCreateTexture(self);

	self->yuv8_shader = GXCreateShader(yuv8_vert, yuv8_frag);
	self->yuv8_shader_tex = glGetUniformLocation(self->yuv8_shader, "frame");
	self->yuv8_shader_brightness = glGetUniformLocation(self->yuv8_shader, "brightness");
	self->yuv8_shader_interpolate = glGetUniformLocation(self->yuv8_shader, "interpolate");

	self->yuv10_shader = GXCreateShader(yuv10_vert, yuv10_frag);
	self->yuv10_shader_tex = glGetUniformLocation(self->yuv10_shader, "frame");
	self->yuv10_shader_size = glGetUniformLocation(self->yuv10_shader, "frame_size");
	self->yuv10_shader_brightness = glGetUniformLocation(self->yuv10_shader, "brightness");
	self->yuv10_shader_interpolate = glGetUniformLocation(self->yuv10_shader, "interpolate");
}

void GXCreateBuffers(GXRenderer* self)
{
	glGenVertexArrays(1, &self->quad_vao);
	glBindVertexArray(self->quad_vao);

	GLuint loc = 0;

	glGenBuffers(1, &self->quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, self->quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(loc);
	glVertexAttribPointer(loc , 3, GL_FLOAT, GL_FALSE, 0, 0);
}

void GXCreateTexture(GXRenderer* self)
{
	glGenTextures(1, &self->frame);
	glBindTexture(GL_TEXTURE_2D, self->frame);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
}
