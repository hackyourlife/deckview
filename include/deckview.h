#ifndef __DECKVIEW_H__
#define __DECKVIEW_H__

#include <GL/gl.h>
#include <GL/glext.h>
#include <DeckLinkAPI.h>

typedef struct {
	GLuint	yuv8_shader;
	GLuint	yuv8_shader_tex;
	GLuint	yuv8_shader_brightness;
	GLuint	yuv8_shader_interpolate;

	GLuint	yuv10_shader;
	GLuint	yuv10_shader_tex;
	GLuint	yuv10_shader_size;
	GLuint	yuv10_shader_brightness;
	GLuint	yuv10_shader_interpolate;

	GLuint	frame;

	GLuint	quad_vao;
	GLuint	quad_vbo;
} GXRenderer;

bool	GXInit(IDeckLink* device);
void	GXMain(void);
void	GXDestroy(void);

GLuint	GXCompileShader(GLuint type, const char* src);
GLuint	GXCreateShader(const char* vs_src, const char* fs_src);

void	GXRendererInit(GXRenderer* self);
void	GXCreateBuffers(GXRenderer* self);
void	GXCreateTexture(GXRenderer* self);

bool	AXInit(unsigned int channels, unsigned int bit);
void	AXStart(void);
void	AXPlay(void* data, size_t size);
void	AXStop(void);
void	AXDestroy(void);

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback
{
	public:
		DeckLinkCaptureDelegate() : refcnt(1) { }

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) {
			return E_NOINTERFACE;
		}

		virtual ULONG STDMETHODCALLTYPE AddRef(void) {
			return __sync_add_and_fetch(&refcnt, 1);
		}

		virtual ULONG STDMETHODCALLTYPE Release(void) {
			unsigned int new_refcnt = __sync_sub_and_fetch(&refcnt, 1);
			if(new_refcnt == 0) {
				delete this;
				return 0;
			}
			return new_refcnt;
		}

		virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
		virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

	private:
		unsigned int	refcnt;
};

#endif
