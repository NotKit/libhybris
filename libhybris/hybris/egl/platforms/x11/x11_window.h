/*
* Copyright (C) 2013 libhybris
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#ifndef FBDEV_WINDOW_H
#define FBDEV_WINDOW_H

#include "nativewindowbase.h"
#include <linux/fb.h>
#include <hardware/gralloc.h>
#include <X11/Xlib.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <list>


class X11NativeWindowBuffer : public BaseNativeWindowBuffer {
friend class X11NativeWindow;

protected:
	X11NativeWindowBuffer(alloc_device_t* alloc,
							unsigned int width,
							unsigned int height,
							unsigned int format,
							unsigned int usage) ;
virtual ~X11NativeWindowBuffer() ;

protected:
	int busy;
	int status;
	alloc_device_t* m_alloc;
};


class X11NativeWindow : public BaseNativeWindow {
public:
	X11NativeWindow(alloc_device_t* alloc, gralloc_module_t* gralloc,
					Display* xl_display, Window xl_window);
		//framebuffer_device_t* fbDev);
	~X11NativeWindow();

	// overloads from BaseNativeWindow
	virtual int setSwapInterval(int interval);
protected:

	virtual int dequeueBuffer(BaseNativeWindowBuffer** buffer, int* fenceFd);
	virtual int queueBuffer(BaseNativeWindowBuffer* buffer, int fenceFd);
	virtual int cancelBuffer(BaseNativeWindowBuffer* buffer, int fenceFd);
	virtual int lockBuffer(BaseNativeWindowBuffer* buffer);
	int postBuffer(ANativeWindowBuffer *buffer);

	virtual unsigned int type() const;
	virtual unsigned int width() const;
	virtual unsigned int height() const;
	virtual unsigned int format() const;
	virtual unsigned int defaultWidth() const;
	virtual unsigned int defaultHeight() const;
	virtual unsigned int queueLength() const;
	virtual unsigned int transformHint() const;
	// perform calls
	virtual int setUsage(int usage);
	virtual int setBuffersFormat(int format);
	virtual int setBuffersDimensions(int width, int height);
	virtual int setBufferCount(int cnt);

private:
	void destroyBuffers();
	void reallocateBuffers();

private:
	Display* m_display;
	Window m_window;
	int m_width, m_height;
	XImage *m_image;
	XShmSegmentInfo m_shminfo;
	GC m_gc;
	bool m_useShm;

	alloc_device_t* m_alloc;
	gralloc_module_t* m_gralloc;
	unsigned int m_usage;
	unsigned int m_bufFormat;
	unsigned int m_bufferCount;
	int m_freeBufs;
	bool m_allocateBuffers;

	std::list<X11NativeWindowBuffer*> m_bufList;
	X11NativeWindowBuffer* m_frontBuf;
};

#endif
// vim: noai:ts=4:sw=4:ss=4:expandtab
