#include <opencv4/opencv2/opencv.hpp>
#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>


#define FPS 5
#define THRESH 64
#define CONT 4

#define RESIZE

size_t VidW, VidH, VidFrames;


struct opf {
	FILE* f = NULL;
	int fd = 0;
	char *path = NULL;
	size_t sz = 0;
};


struct {
	short CLevel = 1;
	struct opf In;
	struct opf Out;
	bool Decompress = false;
	bool DisplayHelp = false;
} Arg;



namespace compressor {
	cv::VideoCapture *video = NULL;
	typedef bool Frame;
	
	void conv_frame (cv::Mat *frame) {
		float alpha = 1, beta = 0;
		
		switch (frame -> type () & CV_MAT_DEPTH_MASK) {
			case CV_8U:
				//alpha = 255.0 / 255.0;
				//beta = 0 * alpha;
				goto skip_color_conv; // dont need to convert to same dest
			
			case CV_8S:
				alpha = 255.0 / 255.0;
				beta = 128.0 / alpha;
				break;
			
			case CV_16U:
				alpha = 255.0 / 65535.0;
				//beta = 0 * alpha;
				break;
			
			case CV_16S:
				alpha = 255.0 / 65535.0;
				//beta = 0 * alpha;
				break;
			
			case CV_32S:
				alpha = 255.0 / 4294967296.0;
				beta = 128.0 / alpha;
				break;
				
			case CV_32F:
				alpha = 255.0 / 1.0;
				//beta = 0 * alpha;
				break;
			
			case CV_64F:
				alpha = 255.0 / 1.0;
				//beta = 0 * alpha;
				break;
		}
		
		frame -> convertTo (*frame, CV_8UC1, alpha, beta);

	skip_color_conv:
		return;
	}


	Frame **newframe (void) {
		Frame **frame = new Frame* [VidW + 1];
		
		for (size_t x = 0; x <= VidW; x ++)
			frame [x] = new Frame [VidH + 1];
		
		return frame;
	}

	void to_monochrome (cv::Mat *frame, Frame **out) {
		uint8_t *pdat = (uint8_t*) frame -> data;
		
		#define px(x,y) pdat [(((x) * VidH) + (y)) * 3]
		int err = 0;
		for (size_t x = 0; x != VidW; x ++) {
			for (size_t y = 0; y != VidH; y ++) { // TODO: dither
				err -= px (x, y);
				if (err < THRESH) {
					out [x] [y] = true;
					err += THRESH * CONT;
				} else {
					out [x] [y] = false;
					//err += THRESH;
				}
			}
		}	
		#undef px	
	}


	void savevid (Frame ***vid) {
		
		struct libdeflate_compressor *compressor = libdeflate_alloc_compressor (Arg.CLevel);
		
		size_t uncompressedvideo_sz = ((VidFrames * VidW * VidH) / 8) + 10;
		size_t uncompressedvideo_left = uncompressedvideo_sz;
		uint8_t *uncompressedvideo = new uint8_t [uncompressedvideo_sz];
		
		short bit = 0; uint8_t *byte = uncompressedvideo;
		*byte = 0;
		for (size_t f = 0; f != VidFrames; f ++) {
			for (size_t x = 0; x != VidW; x ++) {
				for (size_t y = 0; y != VidH; y ++) {
				
					*byte = (*byte << 1) | vid [f] [x] [y];
					
					
					if (++ bit == 8) {
						byte ++;
						*byte = 0;
						if (-- uncompressedvideo_left == 0)
							exit (-1);
						
						bit = 0;
					}
					
				}
			}		
		}
		
		size_t compressedvideo_sz = uncompressedvideo_sz;
		uint8_t *compressedvideo = new uint8_t [compressedvideo_sz];
		
		size_t compressedvideo_len =
			libdeflate_deflate_compress (compressor,
				uncompressedvideo, (uncompressedvideo_sz - uncompressedvideo_left) - 1,
				compressedvideo, compressedvideo_sz
			)
		;
		
		{	uint16_t w = VidW, h = VidH;
			fwrite (&w, sizeof (w), 1, Arg.Out.f);
			fwrite (&h, sizeof (h), 1, Arg.Out.f);
		}
		fwrite (compressedvideo, sizeof (uint8_t), compressedvideo_len, Arg.Out.f);
		
		delete [] uncompressedvideo; delete [] compressedvideo;
		
		libdeflate_free_compressor (compressor);
	}


	void compress (void) {
		fprintf (stderr, "Opening video file %s...", Arg.In.path);
		
		video = new cv::VideoCapture (Arg.In.path);
		
		if (video -> isOpened () == false) {
			fprintf (stderr, "\nVideo file failed to open!\n");
			exit (1);
		}
		
		fprintf (stderr, "Done\nConverting video...");
		
		VidFrames = video -> get (cv::CAP_PROP_FRAME_COUNT);
		Frame ***outvid = new Frame** [VidFrames + 1];
		
		{	cv::Mat frame;
			for (size_t frameno = 0; video -> read (frame); frameno ++) {
				VidW = frame.cols; VidH = frame.rows;
				
				conv_frame (&frame);
				
				outvid [frameno] = newframe ();
				to_monochrome (&frame, outvid [frameno]);
			}
		}
		
		
		fprintf (stderr, "Done\nCompressing and saving video...");
		Arg.Out.f = fopen (Arg.Out.path, "w");
		savevid (outvid);
		fclose (Arg.Out.f);
		fprintf (stderr, "Done\n");
	}
}





namespace decompressor {
	SDL_Window *Window;
	SDL_Surface *WindowSurface, *DrawSurface;
	SDL_Rect Letterbox;
	SDL_Event Event;
	uint32_t *DrawBuffer;
	uint8_t *Video;
	uint8_t *_nextbit_pos;
	
	void setupwindow (void) {
		SDL_Init (SDL_INIT_VIDEO);
		
		Window = SDL_CreateWindow (
			Arg.In.path,
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			VidW, VidH,
			SDL_WINDOW_RESIZABLE
		);
		
		WindowSurface = SDL_GetWindowSurface (Window);
		Letterbox = { 0,0, int (VidW), int (VidH) };
		
		DrawSurface = SDL_CreateRGBSurface (0, VidW,VidH, 32, 0,0,0,0);
		/*{	const SDL_Color pal [2] = {
				{ 0, 0, 0, 255 },
				{ 255, 255, 255, 255}
			};
			
			SDL_SetPaletteColors (DrawSurface -> format -> palette, pal, 0, 2);
		}*/
		DrawBuffer = (uint32_t*) DrawSurface -> pixels;
	}
	
	
	
	uint8_t *inflate (void *data, size_t len) {
		struct libdeflate_decompressor *decompressor = libdeflate_alloc_decompressor ();
		size_t allocsz = Arg.In.sz;
		size_t outsz;
		void *mem = malloc (allocsz);
		
		// attempt decompress
		while (libdeflate_deflate_decompress (decompressor, data, len, mem, allocsz, &outsz) == LIBDEFLATE_INSUFFICIENT_SPACE)
			mem = realloc (mem, (allocsz *= 1.25));
		
		mem = realloc (mem, outsz + 1);
		VidFrames = ((outsz * 8) / (VidW * VidH));
		
		libdeflate_free_decompressor (decompressor);
		return (uint8_t*) mem;
	}
	
	
	void loadfile (void) {
		Arg.In.f = fopen (Arg.In.path, "r");
		{	uint16_t w, h;
			fread (&w, sizeof (w), 1, Arg.In.f);
			fread (&h, sizeof (h), 1, Arg.In.f);
			VidW = w; VidH = h;
		}
		
		// get file size
		size_t actualsz;
		{	Arg.In.fd = fileno (Arg.In.f);
		
			struct stat st;
			fstat (Arg.In.fd, &st);
	
			actualsz = st.st_size;
			Arg.In.sz = actualsz - ftell (Arg.In.f);
		}
		
		// mmap
		void *dat = mmap (
				NULL, actualsz,
				PROT_READ, MAP_PRIVATE,
				Arg.In.fd, 0
			) + ftell (Arg.In.f)
		;
		
		_nextbit_pos = Video = inflate (dat, Arg.In.sz);
		
		//cleanup
		munmap (dat, actualsz);
		fclose (Arg.In.f);
	}
	
	bool nextbit (void) {
		static short bit = 0;
		static uint8_t byte = 0;
		if (++ bit == 8) {
			bit = 0;
			byte = *_nextbit_pos ++;
		} else
			byte = byte << 1;
		
		return byte & 0x80;
	}
	
	
	
	void displayframe (void) {
		for (unsigned int y = 0; y != VidH; y ++)
			for (unsigned int x = 0; x != VidW; x ++)
				DrawBuffer [(y * VidW) + x] = nextbit () ? 0xFFFFFFFF : 0xFF000000;
	}
	
	
	void waitframe (void) {
		static unsigned long last;
		unsigned long
			target = last + (1000 / FPS),
			now = SDL_GetTicks ();
		if (target > now) SDL_Delay (target - now);
		last = target;
	}
	
	void events (void) {
		if (Event.type == SDL_WINDOWEVENT) {
			switch (Event.window.event) {
				case SDL_WINDOWEVENT_RESIZED:
					Letterbox = { 0,0, Event.window.data1, Event.window.data2 };
					
					SDL_SetWindowSize (Window, Event.window.data1, Event.window.data2);
					SDL_UpdateWindowSurface (Window);
					
					WindowSurface = SDL_GetWindowSurface (Window);
					break;
			}
		} else
		if (Event.type == SDL_QUIT) {
			exit (0);
		}
	}
	
	
	void decompress (void) {
		loadfile ();
		setupwindow ();
		
		for (size_t i = 0; i != VidFrames; i ++) {
			while (SDL_PollEvent (&Event)) events ();
			displayframe ();
			
			SDL_BlitScaled (DrawSurface, NULL, WindowSurface, &Letterbox);
			SDL_UpdateWindowSurface (Window);
			
			waitframe ();
		}
	}
}



void displayhelp (const char* argv1) {
	printf ("Usage: %s [-c LEVEL] [-i] INFILE [[-o] OUTFILE]\n", argv1);
	printf ("       %s [-d] [-i] FILE\n", argv1);
	printf ("\n");
	printf ("Standard options\n");
	printf (" -i IF  Select the file to be played/compressed\n");
	printf ("Options for compression\n");
	printf (" -c LV  Compress INFILE at level LV (integer 0-12)\n");
	printf (" -o OF  Select the outputted file from the compression\n");
	printf ("Options for playback\n");
	printf (" -d     Enables playback mode\n");
}


	
int main (int argc, char **argv) {

	for (int arg = 1; arg < argc; arg ++) {
	
		if (argv [arg] [0] == '-') {
			
			for (char *i = &argv [arg] [1]; *i != '\0'; i ++) {
				switch (*i) {
					case 'i':
						Arg.In.path = argv [++ arg];
						break;
					
					case 'o':
						Arg.Out.path = argv [++ arg];
						break;
						
					case 'c':
						Arg.CLevel = atoi (argv [++ arg]);
						Arg.Decompress = false;
						break;
					
					case 'd':
						Arg.Decompress = true;
						break;
					
					case 'h':
						Arg.DisplayHelp = true;
						break;
						
					default:
						fprintf (stderr, "Unknown option %c\n", *i);
						exit (1);
						break;
				}
			}
			
		} else {// is a file?
		
			if (Arg.In.path == NULL) // input path set?
				Arg.In.path = argv [arg];
			else
			if (Arg.Out.path == NULL) // output path set?
				Arg.Out.path = argv [arg];
		
		}
	
	}
	
	
	if (Arg.Out.path == NULL)
		// assume decompress
		Arg.Decompress = true;
	
	if (Arg.In.path == NULL)
		Arg.DisplayHelp = true;
		
	
	
	if (Arg.DisplayHelp) {
		displayhelp (argv [0]);
		exit (0);
	} else
	if (Arg.Decompress) {
		decompressor::decompress ();
	} else {
		compressor::compress ();
	}
}
