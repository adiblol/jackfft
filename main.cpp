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
size_t buffer_size, buffer_jack_size, buffer_wnd_size;
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
	buffer_size_changed = true;
	buffer_locked = false;
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
			if (buffer_jack_size>buffer_wnd_size) {
				buffer_size = buffer_wnd_size;
			} else {
				buffer_size = buffer_jack_size;
			}
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
					window[i] = 0.54 - (0.46 * cos( 2 * M_PI * (i / ((buffer_size - 1) * 1.0))));
				}
				buffer_size_changed = false;
			}
			for (unsigned int i=0;i<buffer_size;i++) {
				buff_fft_in[i] *= window[i];
			}
			fftwf_execute(fft);
	
		}

			
		usleep(20000);
		glFinish();

		if (event.type==SDL_QUIT) break;
		if (event.type==SDL_VIDEORESIZE) {
			window_resize(event.resize.w, event.resize.h);
		}

			
		glClear(GL_COLOR_BUFFER_BIT);

		glBegin(GL_QUADS);
/*		glVertex2f(0.5, 1); //top
		glVertex2f(0, 0.5); //left
		glVertex2f(0.5, 0); //bottom
		glVertex2f(1, 0.5); //right
*/
		for (unsigned int i=0;i<buffer_size;i++) {
			float x1, x2, v, y;
			x1 = ((float)i)/((float)buffer_size);
			x2 = x1 + 1.0/(float)buffer_size;
			v = (logf(fabsf(buff_fft_out[i]/(float)buffer_size))/M_LN10+4.2f)/4.8f;
			if (buff_disp_prev[i]<v) {
				buff_disp_prev[i] = v;
			} else {
				buff_disp_prev[i] -= 0.015f;
				if (buff_disp_prev[i]<0) buff_disp_prev[i] = 0;
			}
			//y = fabsf(buff_fft_out[i]);
			/*if (v>fft_peak) {
				printf("New FFT peak: %f\n", v);
				fft_peak = v;
			}*/
			y = buff_disp_prev[i];
			glColor4f(1, y, 0, 1);
			glVertex2f(x1, 0);
			glVertex2f(x2, 0);
			glVertex2f(x2, y);
			glVertex2f(x1, y);
			
			glColor4f(1,1,1, 0.2);
			glVertex2f(x1, 0);
			glVertex2f(x2, 0);
			glVertex2f(x2, v);
			glVertex2f(x1, v);
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

