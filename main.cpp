
/*
	jackfft
	OpenGL-accelerated spectrum viewer for JACK
	
	Copyright (C) 2011 adiblol
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sched.h>
#include <sys/types.h>
#include <jack/jack.h>
#include <math.h>
#include <fftw3.h>
#include <SDL/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>

jack_client_t* client;
jack_port_t* jackport;

typedef float sample;
const size_t buffer_max = 16384;
size_t buffer_size, buffer_jack_size, buffer_wnd_size, wnd_width;
sample *buffer_1, *buffer_2, *buffer_input;
bool buffer_locked, buffer_size_changed;


int process(jack_nframes_t nframes, void* data) {
	jack_default_audio_sample_t* buff;
	buff = (sample*)jack_port_get_buffer(jackport, nframes);
	if (nframes>buffer_max) return 0; // FIXME: should report error
	if (!buffer_locked) { // if locked just abort writing, don't hang RT process waiting for unlock.
		buffer_locked = true;
		if (buffer_jack_size!=nframes) {
			buffer_size_changed = true;
			buffer_jack_size = nframes;
		}
		if (nframes>buffer_size) nframes = buffer_size;
		memcpy(buffer_input, buff, nframes*sizeof(sample));
		buffer_locked = false;
	}
	return 0;
}

void window_resize(int width, int height) {
	if (height==0) height = 1;

	SDL_SetVideoMode(width, height, 32, SDL_OPENGL|SDL_RESIZABLE);

	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, 0, 1);

	while(buffer_locked) sched_yield();
	buffer_locked = true;
	buffer_wnd_size = 1;
	while (buffer_wnd_size<(width/2)) {
		buffer_wnd_size *= 2;
	}
	wnd_width = width;
	buffer_size_changed = true;
	buffer_locked = false;
}

void color_from_value(float v, float &r, float &g, float &b) {
	//v=1.0-v;
	if (v<0.0) { r=1.0+v; g=0; b=0; }                // -> red
	if (v<0.2) { r=1; g=v*5.0; b=0; } else           // red -> yellow
	if (v<0.4) { r=1-(v-0.2)*5.0; g=1; b=0; } else   // yellow -> green
	if (v<0.6) { r=0; g=1; b=(v-0.4)*5.0; } else     // green -> turq
	if (v<0.8) { r=0; g=1-(v-0.6)*5.0; b=1; } else   // turq -> blue
	if (v<1.0) { r=(v-0.8)*5.0; g=0; b=1; } else {   // blue -> purple
		r = 1;
		g = v-1.0;
		b = 1;
	}
}

int main(int argc, char** argv) {
	char client_name[64];
	if (argc>1) {
		strncpy(client_name, argv[1], 63);
	} else {
		snprintf(client_name, 63, "jackfft_%i", getpid());
	}

	jack_status_t status;
	client = jack_client_open(client_name, JackNullOption, &status, NULL);
	if (client==NULL) exit(8);

	jack_set_process_callback(client, process, NULL);
	jackport = jack_port_register(client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

	
	//SDL_Surface* screen;
	SDL_Init(SDL_INIT_VIDEO);
	SDL_SetVideoMode(480, 360, 32, SDL_OPENGL|SDL_RESIZABLE);
	window_resize(480, 360);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	//SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	SDL_WM_SetCaption(client_name, NULL);
	
	glDisable(GL_DEPTH_TEST);
	glMatrixMode(GL_PROJECTION);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_POLYGON_SMOOTH);

	bool working = true;
	SDL_Event event;


	buffer_locked = false;
	buffer_size = 0;
	buffer_jack_size = 0;
	buffer_size_changed = false;
	buffer_1 = new sample[buffer_max];
	buffer_2 = new sample[buffer_max];
	sample* window = new sample[buffer_max];
	sample* buff_fft_in  = (sample*)fftwf_malloc(sizeof(sample)*buffer_max);
	sample* buff_fft_out = (sample*)fftwf_malloc(sizeof(sample)*buffer_max);
	sample* buff_disp_prev = new sample[buffer_max];
	float* x_scale = new float[buffer_max+1];
	bool* lines_occupied = new bool[buffer_max];
	memchr(buff_disp_prev, 0, sizeof(sample)*buffer_max);
	fftwf_plan fft;
	bool fft_alloc = false;
	buffer_input = buffer_1;

	jack_activate(client);

	/*glLoadIdentity();
	glOrtho(0, 1, 0, 1, 0, 1);
	glColor4f(1, 1, 1, 1);*/
	sample fft_peak = 0;
	while(true) {
		SDL_PollEvent(&event);
		if ((SDL_GetAppState() && SDL_APPACTIVE) == 0) {
			usleep(100000);
			continue;
		}

		while (buffer_locked) sched_yield();
		buffer_locked = true;
		memcpy(buff_fft_in, buffer_input, buffer_size*sizeof(sample));
		buffer_locked = false;
		if (buffer_size_changed) {
			/*if (buffer_jack_size>buffer_wnd_size) {
				buffer_size = buffer_wnd_size;
			} else {
				buffer_size = buffer_jack_size;
			}*/
			buffer_size = buffer_jack_size;

		}

		if (buffer_size!=0) {
			if (buffer_size_changed) {	
				if (fft_alloc) {
					fftwf_destroy_plan(fft);
				}
				fft = fftwf_plan_r2r_1d(buffer_size, buff_fft_in, buff_fft_out,
					FFTW_REDFT00, FFTW_ESTIMATE);
				fft_alloc = true;
				for (unsigned int i=0;i<buffer_size;i++) {
					//window[i] = 0.54 - (0.46 * cos( 2 * M_PI * (i / ((buffer_size - 1) * 1.0))));
					window[i] = 0.5*( 1.0 - cos(2.0*M_PI*(float)i/(float)(buffer_size-1)) ); // Hann
				}
				buffer_size_changed = false;
				for (unsigned int i=0;i<buffer_size;i++) {
					x_scale[i] = logf(i/(float)buffer_size)/7.0f+1.0f;
				}
				x_scale[buffer_size] = 1;

			}
			for (unsigned int i=0;i<buffer_size;i++) {
				buff_fft_in[i] *= window[i];
			}
			fftwf_execute(fft);
	
		};

			
		usleep(20000);
		glFinish();

		if (event.type==SDL_QUIT) break;
		if (event.type==SDL_VIDEORESIZE) {
			window_resize(event.resize.w, event.resize.h);
		}
		if (buffer_size==0) continue;
			
		glClear(GL_COLOR_BUFFER_BIT);

		glBegin(GL_QUADS);
/*		glVertex2f(0.5, 1); //top
		glVertex2f(0, 0.5); //left
		glVertex2f(0.5, 0); //bottom
		glVertex2f(1, 0.5); //right
*/
//		float next_x = 0;
		memset(lines_occupied, 0, wnd_width*sizeof(bool));
		for (unsigned int i=1;i<buffer_size-1;i++) {
			float x1, x2, v, y;
			//x1 = (logf(((float)i)/((float)buffer_size))+7.0f)/7.0f;
			//x1 = next_x;
			//x2 = (logf((i+1)/(float)buffer_size)+7.0f)/7.0f;
			x1 = x_scale[i];
			x2 = x_scale[i+1];
			//if (floor(x_scale[i-1]*wnd_width)==floor(x2*wnd_width)) continue;
			//next_x = x2;
			/*int line = floor(x1*(float)wnd_width);
			if ((line>=0) && (line<wnd_width)) {
				if (lines_occupied[line]) continue;
				lines_occupied[line] = true;
			};*/
			v = (logf(fabsf(buff_fft_out[i]/(float)buffer_size))/M_LN10+4.2f)/4.8f;
			//if ((v>1000.0f) || (v<0.0f)) continue;
			if (buff_disp_prev[i]<v) {
				buff_disp_prev[i] = v;
			} else {
				buff_disp_prev[i] -= 0.015f;
				if (buff_disp_prev[i]<0) buff_disp_prev[i] = 0;
			}
			//if (v>1.0f) continue;
			//y = fabsf(buff_fft_out[i]);
			/*if (v>fft_peak) {
				printf("New FFT peak: %f\n", v);
				fft_peak = v;
			}*/
			float c_r, c_g, c_b;

			y = (buff_disp_prev[i]+buff_disp_prev[i-1]/2.0f+buff_disp_prev[i+1]/2.0f)/2.0f;
			color_from_value(x1+y*0.3, c_r, c_g, c_b);
			if ((v>0) && (v<1)) {
				glColor4f(c_r,c_g,c_b, y/2.0);
				glVertex2f(x1, 0);
				glVertex2f(x2, 0);
				glVertex2f(x2, v);
				glVertex2f(x1, v);
			}

			if ((y>0) && (y<1)) {
				//glColor4f(1, y, 0, 1);
				glColor4f(c_r, c_g, c_b, y*0.6+0.4);
				glVertex2f(x1, 0);
				glVertex2f(x2, 0);
				glColor4f(c_r, c_g, c_b, y);
				glVertex2f(x2, y);
				glVertex2f(x1, y);
			}
		}
		glEnd();
		glFlush();
		SDL_GL_SwapBuffers();
	}
	
	fftwf_destroy_plan(fft);
	jack_client_close(client);
	SDL_Quit();

	return 0;
}

