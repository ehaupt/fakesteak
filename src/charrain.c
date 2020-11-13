#include <stdio.h>      // fprintf(), stdout, setlinebuf()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE, rand()
#include <stdint.h>     // uint8_t, uint16_t
#include <unistd.h>     // getopt(), STDOUT_FILENO
#include <math.h>       // ceil()
#include <time.h>       // time(), nanosleep(), struct timespec
#include <signal.h>     // sigaction(), struct sigaction
#include <termios.h>    // struct winsize 
#include <sys/ioctl.h>  // ioctl(), TIOCGWINSZ

// program information

#define PROGRAM_NAME "charrain"
#define PROGRAM_URL  "https://github.com/domsson/charrain"

#define PROGRAM_VER_MAJOR 0
#define PROGRAM_VER_MINOR 1
#define PROGRAM_VER_PATCH 0

// do not change these 

#define ANSI_FONT_RESET "\x1b[0m"
#define ANSI_FONT_BOLD  "\x1b[1m"
#define ANSI_FONT_FAINT "\x1b[2m"

#define ANSI_HIDE_CURSOR "\e[?25l"
#define ANSI_SHOW_CURSOR "\e[?25h"

#define BITMASK_ASCII 0x00FF
#define BITMASK_STATE 0x0300
#define BITMASK_TSIZE 0xFC00

#define STATE_NONE 0
#define STATE_DROP 1
#define STATE_TAIL 2

#define DEBUG_ASCII 1
#define DEBUG_STATE 2
#define DEBUG_TSIZE 3

#define TSIZE_MIN 8
#define TSIZE_MAX 63

#define ASCII_MIN 32
#define ASCII_MAX 126

// these can be tweaked if need be

#define ERROR_FACTOR_MIN 0.01
#define ERROR_FACTOR_MAX 0.10
#define ERROR_FACTOR_DEF 0.02

#define DROPS_FACTOR_MIN 0.01
#define DROPS_FACTOR_MAX 0.10
#define DROPS_FACTOR_DEF 0.0001

#define SPEED_FACTOR_MIN 0.01
#define SPEED_FACTOR_MAX 1.00
#define SPEED_FACTOR_DEF 0.10

// rain colors (8 bit codes)
//
// index 0 is the color for the drop, the remaining colors
// will be used for the tail, starting from index 1 for the 
// cell closest to the drop and the last color being used 
// for the cell furthest from the drop
//
// https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit

static uint8_t colors[] = { 231, 48, 41, 35, 29, 238 };

// make sure this matches the number of elements in `colors`

//#define NUM_COLORS 6
#define NUM_COLORS sizeof(colors) / sizeof(colors[0])

// these are flags used for signal handling

static volatile int resized;   // window resize event received
static volatile int running;   // controls running of the main loop 
static volatile int handled;   // last signal that has been handled 

//
//  the matrix' data represents a 2D array of size cols * rows.
//  every data element is a 16 bit int which stores information
//  about that matrix cell as follows:
//
//  128 64  32  16   8   4   2   1  128 64  32  16   8   4   2   1
//   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |
//   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0
//  '---------------------' '-----' '-----------------------------'
//          TSIZE            STATE               ASCII
//
//  ASCII: the char code to display (values 32 through 126)
//  STATE: 0 for NONE, 1 for DROP or 2 for TAIL
//  TSIZE: length of tail (for DROP) or color intensity (for TAIL)
//

typedef struct matrix
{
	uint16_t *data;     // matrix data
	uint16_t  cols;     // number of columns
	uint16_t  rows;     // number of rows
	size_t drop_count;  // current number of drops
	float  drop_ratio;  // desired ratio of drops
}
matrix_s;

typedef struct options
{
	float   speed;         // speed factor
	float   drops;         // drops ratio / factor
	float   error;         // error ratio / factor
	time_t  rand;          // seed for rand()
	uint8_t bg_color;      // custom background color
	uint8_t bg_set : 1;    // set background color
	uint8_t help : 1;      // show help and exit
	uint8_t version : 1;   // show version and exit
}
options_s;

static void
parse_args(int argc, char **argv, options_s *opts)
{
	opterr = 0;
	int o;
	while ((o = getopt(argc, argv, "b:d:e:hr:s:V")) != -1)
	{
		switch (o)
		{
			case 'b':
				opts->bg_set = 1;
				opts->bg_color = atoi(optarg);
				break;
			case 'd':
				opts->drops = atof(optarg);
				break;
			case 'e':
				opts->error = atof(optarg);
				break;
			case 'h':
				opts->help = 1;
				break;
			case 'r':
				opts->rand = atol(optarg);
				break;
			case 's':
				opts->speed = atof(optarg);
				break;
			case 'V':
				opts->version = 1;
				break;
		}
	}
}

static void
help(const char *invocation, FILE *where)
{
	fprintf(where, "USAGE\n");
	fprintf(where, "\t%s [OPTIONS...]\n\n", invocation);
	fprintf(where, "OPTIONS\n");
	fprintf(where, "\t-b\tset background color (0 - 255)\n");
	fprintf(where, "\t-d\tdrops ratio (default is %1.2f)\n", DROPS_FACTOR_DEF);
	fprintf(where, "\t-e\terror ratio (default is %1.2f)\n", ERROR_FACTOR_DEF);
	fprintf(where, "\t-h\tprint this help text and exit\n");
	fprintf(where, "\t-r\tseed for the random number generator\n");
	fprintf(where, "\t-s\tspeed factor (default is %1.2f)\n", SPEED_FACTOR_DEF);
	fprintf(where, "\t-V\tprint version information and exit\n");
}

static void
version(FILE *where)
{
	fprintf(where, "%s %d.%d.%d\n%s\n", PROGRAM_NAME,
			PROGRAM_VER_MAJOR, PROGRAM_VER_MINOR, PROGRAM_VER_PATCH,
			PROGRAM_URL);
}

static void
on_signal(int sig)
{
	switch (sig)
	{
		case SIGWINCH:
			resized = 1;
			break;
		case SIGINT:
		case SIGQUIT:
		case SIGTERM:
			running = 0;
			break;
	}
	handled = sig;
}

static void 
cap_float(float *val, float min, float max)
{
	if (*val < min) { *val = min; return; }
	if (*val > max) { *val = max; return; }
}

static int
rand_int(int min, int max)
{
	return min + rand() % ((max + 1) - min);
}

static float
rand_float()
{
	return (float) rand() / (float) RAND_MAX;
}

static int
rand_int_mincap(int min, int max)
{
	int r = rand() % max;
	return r < min ? min : r;
}

static uint8_t 
rand_ascii()
{
	return rand_int_mincap(ASCII_MIN, ASCII_MAX);
}

static void
color_fg(uint8_t color)
{
	printf("\x1b[38;5;%hhum", color);
}

static void
color_bg(uint8_t color)
{
	printf("\x1b[48;5;%hhum", color);
}

//
// Functions to manipulate individual matrix cell values
//

static uint16_t
val_new(uint8_t ascii, uint8_t state, uint8_t tsize)
{
	return (BITMASK_TSIZE & (tsize << 10)) | (BITMASK_STATE & (state << 8)) | ascii;
}

static uint8_t
val_get_ascii(uint16_t value)
{
	return value & BITMASK_ASCII;
}

static uint8_t
val_get_state(uint16_t value)
{
	return (value & BITMASK_STATE) >> 8;
}

static uint8_t
val_get_tsize(uint16_t value)
{
	return (value & BITMASK_TSIZE) >> 10;
}

//
// Functions to access / set matrix values
//

static int
mat_idx(matrix_s *mat, int row, int col)
{
	return row * mat->cols + col;
}

static uint16_t
mat_get_value(matrix_s *mat, int row, int col)
{
	if (row >= mat->rows) return 0;
	if (col >= mat->cols) return 0;
	return mat->data[mat_idx(mat, row, col)];
}

static uint8_t
mat_get_ascii(matrix_s *mat, int row, int col)
{
	return val_get_ascii(mat_get_value(mat, row, col));
}

static uint8_t
mat_get_state(matrix_s *mat, int row, int col)
{
	return val_get_state(mat_get_value(mat, row, col));
}

static uint8_t
mat_get_tsize(matrix_s *mat, int row, int col)
{
	return val_get_tsize(mat_get_value(mat, row, col));
}

static uint8_t
mat_set_value(matrix_s *mat, int row, int col, uint16_t value)
{
	if (row >= mat->rows) return 0;
	if (col >= mat->cols) return 0;
	return mat->data[mat_idx(mat, row, col)] = value;
}

static uint8_t
mat_set_ascii(matrix_s *mat, int row, int col, uint8_t ascii)
{
	uint16_t value = mat_get_value(mat, row, col);
	return mat_set_value(mat, row, col, 
			val_new(ascii, val_get_state(value), val_get_tsize(value)));
}

static uint8_t
mat_set_state(matrix_s *mat, int row, int col, uint8_t state)
{
	uint16_t value = mat_get_value(mat, row, col);
	uint8_t  tsize = state == STATE_NONE ? 0 : val_get_tsize(value);
	return mat_set_value(mat, row, col, 
			val_new(val_get_ascii(value), state, tsize));
}

static uint8_t
mat_set_tsize(matrix_s *mat, int row, int col, uint8_t tsize)
{
	uint16_t value = mat_get_value(mat, row, col);
	return mat_set_value(mat, row, col, 
			val_new(val_get_ascii(value), val_get_state(value), tsize));
}

//
// Functions to create, manipulate and print a matrix
//

static void
mat_glitch(matrix_s *mat, float fraction)
{
	int size = mat->rows * mat->cols;
	int num = fraction * size;

	int row = 0;
	int col = 0;

	for (int i = 0; i < num; ++i)
	{
		row = rand() % mat->rows;
		col = rand() % mat->cols;
		mat_set_ascii(mat, row, col, rand_ascii());
	}
}

static void
mat_print(matrix_s *mat)
{
	uint16_t value = 0;
	uint8_t  state = STATE_NONE;

	size_t size = mat->cols * mat->rows;

	for (int i = 0; i < size; ++i)
	{
		value = mat->data[i];
		state = val_get_state(value);

		switch (state)
		{
			case STATE_NONE:
				fputc(' ', stdout);
				break;
			case STATE_DROP:
				color_fg(colors[0]);
				fputc(val_get_ascii(value), stdout);
				break;
			case STATE_TAIL:
				color_fg(colors[val_get_tsize(value)]);
				fputc(val_get_ascii(value), stdout);
				break;
		}
	}

	// Depending on what type of buffering we use, flushing might be needed
	fflush(stdout);
}

static void
mat_debug(matrix_s *mat, int what)
{
	fprintf(stdout, "\x1b[0m");

	uint16_t value = 0;

	size_t size = mat->cols * mat->rows;

	for (int i = 0; i < size; ++i)
	{
		value = mat->data[i];
		switch (what)
		{
			case DEBUG_STATE:
				fprintf(stdout, "%hhu", val_get_state(value));
				break;
			case DEBUG_ASCII:
				fprintf(stdout, "%c",   val_get_ascii(value));
				break;
			case DEBUG_TSIZE:
				fprintf(stdout, "%hhu", val_get_tsize(value));
				break;
		}
	}
	fflush(stdout);
}

/*
 * Turn the specified cell into a DROP cell.
 */
static void
mat_put_cell_drop(matrix_s *mat, int row, int col, int tsize)
{
	mat_set_state(mat, row, col, STATE_DROP);
	mat_set_tsize(mat, row, col, tsize);
}

/*
 * Turn the specified cell into a TAIL cell.
 */
static void
mat_put_cell_tail(matrix_s *mat, int row, int col, int tsize, int tnext)
{
	float intensity = (float) tnext / (float) tsize; // 1 for end of trace, 0.x for beginning
	int color = ceil((NUM_COLORS-1) * intensity);
	mat_set_state(mat, row, col, STATE_TAIL);
	mat_set_tsize(mat, row, col, color);
}

/*
 * Add a DROP, including its TAIL cells, to the matrix, 
 * starting from the specified position.
 *
 * TODO make it so it can also draw partial traces, where
 *      the drop is past the bottom row, but some of the
 *      tail cells are still inside the matrix
 */
static void
mat_add_drop(matrix_s *mat, int row, int col, int tsize)
{
	for (int i = 0; i <= tsize; ++i, --row)
	{
		if (row < 0)          break;
		if (row >= mat->rows) continue;

		if (i == 0)
		{
			mat_put_cell_drop(mat, row, col, tsize);
			++mat->drop_count;
		}
		else
		{
			mat_put_cell_tail(mat, row, col, tsize, i);
		}
	}
}

/*
 * Make it rain by adding some DROPs to the matrix.
 */
static void
mat_rain(matrix_s *mat)
{
	int num = (int) (mat->cols * mat->rows) * mat->drop_ratio;

	int c = 0;
	int r = 0;

	for (int i = 0; i < num; ++i)
	{
		c = rand_int(0, mat->cols - 1);
		r = rand_int(0, mat->rows - 1);
		mat_add_drop(mat, r, c, rand_int(TSIZE_MIN, TSIZE_MAX));
	}
}

/*
 * Move every cell down one row, potentially adding a new tail cell at the top.
 * Returns 1 if a DROP 'fell off the bottom', otherwise 0.
 */
static int
mat_mov_col(matrix_s *mat, int col)
{
	uint8_t tail_size = 0;
	uint8_t tail_seen = 0;

	uint16_t value = 0;
	uint8_t  state = STATE_NONE;
	uint8_t  tsize = 0;

	// manually check the bottom-most cell: is it a DROP?
	int dropped = mat_get_state(mat, mat->rows - 1, col) == STATE_DROP;

	// iterate all cells in this column, moving each down one cell
	for (int row = mat->rows - 1; row >= 0; --row)
	{
		// get the current cell's meta data
		value = mat_get_value(mat, row, col);
		state = val_get_state(value);
		tsize = val_get_tsize(value);

		// nothing to do if this cell is neither DROP nor TAIL
		if (state == STATE_NONE)
		{
			continue;
		}

		// move the cell one down
		mat_set_state(mat, row+1, col, state);
		mat_set_tsize(mat, row+1, col, tsize);

		// null the current cell
		mat_set_state(mat, row, col, STATE_NONE);
		mat_set_tsize(mat, row, col, 0);

		// keep track of the tail length of the last seen drop
		if (state == STATE_DROP)
		{
			// remember the tail size to draw for this drop
			tail_size = tsize;
			tail_seen = 0;
		}
		else if (state == STATE_TAIL && tail_size > 0)
		{
			// keep track of how many tail cells we've seen
			++tail_seen;
		}
	}

	// if the top-most cell wasn't empty, we might have to add a tail cell
	if (state != STATE_NONE && tail_seen < tail_size)
	{
		mat_put_cell_tail(mat, 0, col, tail_size, tail_seen + 1); 
	}

	return dropped;
}

static void 
mat_update(matrix_s *mat)
{
	// add new drops at the top, trying to get to the desired drop count

	size_t drops_desired = (mat->cols * mat->rows) * mat->drop_ratio;
	size_t drops_missing = drops_desired - mat->drop_count; 
	size_t drops_to_add  = (size_t) ((float) drops_missing / (float) mat->rows);

	int col = 0;

	for (size_t i = 0; i <= drops_to_add; ++i)
	{
		col = rand_int(0, mat->cols - 1);
		
		mat_add_drop(mat, 0, col, rand_int(TSIZE_MIN, TSIZE_MAX));
	}

	// move each column down one cell, possibly dropping some drops
	for (int col = 0; col < mat->cols; ++col)
	{
		mat->drop_count -= mat_mov_col(mat, col);
	}
}

static void
mat_fill(matrix_s *mat)
{
	for (int r = 0; r < mat->rows; ++r)
	{
		for (int c = 0; c < mat->cols; ++c)
		{
			mat_set_state(mat, r, c, STATE_NONE);
			mat_set_ascii(mat, r, c, rand_ascii());
		}
	}
}

/*
 * Creates or recreates (resizes) the given matrix.
 * Returns -1 on error (out of memory), 0 on success.
 */
static int
mat_init(matrix_s *mat, uint16_t rows, uint16_t cols, float drop_ratio)
{
	mat->data = realloc(mat->data, sizeof(mat->data) * rows * cols);
	if (mat->data == NULL)
	{
		return -1;
	}
	
	mat->rows = rows;
	mat->cols = cols;

	mat->drop_count = 0;
	mat->drop_ratio = drop_ratio;
	
	return 0;
}

void
mat_free(matrix_s *mat)
{
	free(mat->data);
}

int
cli_wsize(struct winsize *ws)
{
#ifdef TIOCGWINSZ
	return ioctl(STDOUT_FILENO, TIOCGWINSZ, ws);
#endif
	return -1;
}

void
cli_clear(int rows)
{
	//printf("\033[%dA", rows); // cursor up 
	//printf("\033[2J"); // clear screen
	printf("\033[H");  // cursor back to top, left
	//printf("\033[%dT", rows); // scroll down
	//printf("\033[%dN", rows); // scroll up
}

void
cli_setup()
{
	fprintf(stdout, ANSI_HIDE_CURSOR);
	fprintf(stdout, ANSI_FONT_BOLD);

	printf("\033[2J"); // clear screen
	printf("\033[H");  // cursor back to top, left
	
	// set the buffering to fully buffered, we're adult and flush ourselves
	setvbuf(stdout, NULL, _IOFBF, 0);
}

void
cli_reset()
{
	fprintf(stdout, ANSI_FONT_RESET);
	fprintf(stdout, ANSI_SHOW_CURSOR);
	
	printf("\033[2J"); // clear screen
	printf("\033[H");  // cursor back to top, left

	setvbuf(stdout, NULL, _IOLBF, 0);
}


/*
 * Some good resources that have helped me with this project:
 *
 * https://youtu.be/MvEXkd3O2ow?t=26
 * https://matrix.logic-wire.de/
 *
 * https://man7.org/linux/man-pages/man4/tty_ioctl.4.html
 * https://en.wikipedia.org/wiki/ANSI_escape_code
 * https://gist.github.com/XVilka/8346728
 * https://stackoverflow.com/a/33206814/3316645 
 * https://jdebp.eu/FGA/clearing-the-tui-screen.html#POSIX
 */
int
main(int argc, char **argv)
{
	// set signal handlers for the usual susspects plus window resize
	struct sigaction sa = { .sa_handler = &on_signal };
	sigaction(SIGINT,   &sa, NULL);
	sigaction(SIGQUIT,  &sa, NULL);
	sigaction(SIGTERM,  &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);

	// parse command line options
	options_s opts = { 0 };
	parse_args(argc, argv, &opts);

	if (opts.help)
	{
		help(argv[0], stdout);
		return EXIT_SUCCESS;
	}

	if (opts.version)
	{
		version(stdout);
		return EXIT_SUCCESS;
	}

	if (opts.speed == 0.0)
	{
		opts.speed = SPEED_FACTOR_DEF;
	}

	if (opts.drops == 0.0)
	{
		opts.drops = DROPS_FACTOR_DEF;
	}

	if (opts.error == 0.0)
	{
		opts.error = ERROR_FACTOR_DEF;
	}

	if (opts.rand == 0)
	{
		opts.rand = time(NULL);
	}

	cap_float(&opts.speed, SPEED_FACTOR_MIN, SPEED_FACTOR_MAX);
	cap_float(&opts.drops, DROPS_FACTOR_MIN, DROPS_FACTOR_MAX);
	cap_float(&opts.error, ERROR_FACTOR_MIN, ERROR_FACTOR_MAX);

	// get the terminal dimensions
	struct winsize ws = { 0 };
	if (cli_wsize(&ws) == -1)
	{
		fprintf(stderr, "Failed to determine terminal size\n");
		return EXIT_FAILURE;
	}

	// this will determine the speed of the entire thing
	int less = 90000000 * opts.speed;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 - less };
	
	// seed the random number generator with the current unix time
	srand(opts.rand);

	// initialize the matrix
	matrix_s mat = { 0 }; 
	mat_init(&mat, ws.ws_row, ws.ws_col, opts.drops);
	mat_fill(&mat);

	// prepare the terminal for our shenanigans
	cli_setup();

	running = 1;
	while(running)
	{
		if (resized)
		{
			cli_wsize(&ws);
			
			// reinitialize the matrix
			mat_init(&mat, ws.ws_row, ws.ws_col, opts.drops);
			mat_fill(&mat);
			mat_rain(&mat);
			resized = 0;
		}

		cli_clear(mat.rows);
		mat_print(&mat);                // print to the terminal
		mat_glitch(&mat, opts.error);   // apply random defects
		mat_update(&mat);               // move all drops down one row
		//mat_debug(&mat, DEBUG_TSIZE);
		nanosleep(&ts, NULL);
	}

	// make sure all is back to normal before we exit
	mat_free(&mat);	
	cli_reset();
	return EXIT_SUCCESS;
}
