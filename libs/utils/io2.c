#include "io2.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define FORMAT_BUFFER_SIZE 128


int is_specifier(const char ch)
{
	return (ch >= '0' && ch <= '9')
		|| ch == 'h' || ch == 'l'
		|| ch == 'j' || ch == 'z' 
		|| ch == 't' || ch == 'L'
		|| ch == '.';
}

int is_integer(const char ch)
{
	return ch == 'd' || ch == 'i';
}

int is_unsigned(const char ch)
{
	return ch == 'u' || ch == 'o'
		|| ch == 'x' || ch == 'X';
}

int is_floating(const char ch)
{
	return ch == 'f' || ch == 'F'
		|| ch == 'e' || ch == 'E'
		|| ch == 'g' || ch == 'G'
		|| ch == 'a' || ch == 'A';
}

int is_characters(const char ch)
{
	return ch == 'c' || ch == 'C'
		|| ch == 's' || ch == 'S'
		|| ch == '[';
}

int is_pointer(const char ch)
{
	return ch == 'p';
}

int is_count(const char ch)
{
	return ch == 'n';
}


void scan_arg(universal_io *const io, const size_t position, const char *const format, size_t size, void *arg)
{
	char buffer[FORMAT_BUFFER_SIZE];
		
	for (size_t i = 0; i < size; i++)
	{
		buffer[i] = format[i];
	}

	buffer[size] = '%';
	buffer[size + 1] = 'n';
	buffer[size + 2] = '\0';

	int number = 0;
	sscanf(io->in_buffer, buffer, arg, &number);
	io->in_position += number;

	if (is_count(format[size - 1]))
	{
		*(int *)arg = io->in_position - position;
		/*switch (format[size - 2])
		{
			case 'h':
				if (format[size - 3] == 'h')
				{
					arg = (void *)va_arg(*args, signed char *);
				}
				else
				{
					arg = (void *)va_arg(*args, short int *);
				}
				break;
			case 'l':
				if (format[size - 3] == 'l')
				{
					arg = (void *)va_arg(*args, long long int *);
				}
				else
				{
					arg = (void *)va_arg(*args, long int *);
				}
				break;
			case 'j':
				arg = (void *)va_arg(*args, intmax_t *);
				//arg = (void *)va_arg(*args, int *);
				break;
			case 'z':
				arg = (void *)va_arg(*args, size_t *);
				break;
			case 't':
				arg = (void *)va_arg(*args, ptrdiff_t *);
				break;
			default:
				arg = (void *)va_arg(*args, int *);
		}*/
	}
}

int in_func_buffer(universal_io *const io, const char *const format, va_list args)
{
	const size_t position = io->in_position;
	int ret = 0;

	size_t last = 0;
	size_t i = 0;
	while (format[i] != '\0')
	{
		if (format[i] == '%')
		{
			while (is_specifier(format[i + 1]) || is_integer(format[i + 1]) || is_unsigned(format[i + 1])
				|| is_floating(format[i + 1]) || is_characters(format[i + 1]) || is_pointer(format[i + 1])
				|| is_count(format[i + 1]))
			{
				i++;
				if (format[i] == '[')
				{
					while (format[i + 1] != '\0' && format[i] != '\\' && format[i + 1] != ']')
					{
						i++;
					}
				}
			}

			if (format[i] != '%' && !is_specifier(format[i]))
			{
				scan_arg(io, position, &format[last], i + 1, va_arg(args, void *));
				last = i + 1;
			}
		}

		i++;
	}

	return ret;
}

int in_func_user(universal_io *const io, const char *const format, va_list args)
{
	return io->in_user_func(format, args);
}


int out_func_user(universal_io *const io, const char *const format, va_list args)
{
	return io->out_user_func(format, args);
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


universal_io io_create()
{
	universal_io io;

	io.in_file = NULL;
	io.in_buffer = NULL;

	io.in_size = 0;
	io.in_position = 0;

	io.in_user_func = NULL;
	io.in_func = NULL;

	io.out_file = NULL;
	io.out_buffer = NULL;

	io.out_size = 0;
	io.out_position = 0;

	io.out_user_func = NULL;
	io.out_func = NULL;

	return io;
}


int in_set_file(universal_io *const io, const char *const path)
{
	if (io == NULL || path == NULL)
	{
		return -1;
	}

	//io->in_file = path;

	return 0;
}

int in_set_buffer(universal_io *const io, const char *const buffer)
{
	if (in_clear(io))
	{
		return -1;
	}

	io->in_buffer = buffer;

	io->in_size = strlen(buffer);
	io->in_position = 0;
	
	io->in_func = &in_func_buffer;

	return 0;
}

int in_set_func(universal_io *const io, const io_user_func func)
{
	if (in_clear(io))
	{
		return -1;
	}
	
	io->in_user_func = func;
	io->in_func = &in_func_user;

	return 0;
}


int in_is_correct(const universal_io *const io)
{
	return io != NULL && (in_is_file(io) || in_is_buffer(io) || in_is_func(io));
}

int in_is_file(const universal_io *const io)
{
	return io != NULL && io->in_file != NULL;
}

int in_is_buffer(const universal_io *const io)
{
	return io != NULL && io->in_buffer != NULL;
}

int in_is_func(const universal_io *const io)
{
	return io != NULL && io->in_user_func != NULL;
}


io_func in_get_func(const universal_io *const io)
{
	return io != NULL ? io->in_func : NULL;
}

size_t in_get_path(const universal_io *const io, char *const buffer)
{
	if (!in_is_file(io))
	{
		return 0;
	}

	//sprintf(buffer, "%s", io->in_file);
	return strlen(buffer);
}

const char *in_get_buffer(const universal_io *const io)
{
	return in_is_buffer(io) ? io->in_buffer : NULL;
}

size_t in_get_position(const universal_io *const io)
{
	return in_is_buffer(io) ? io->in_position : 0;
}


int in_close_file(universal_io *const io)
{
	return 0;
}

int in_clear(universal_io *const io)
{
	if (io == NULL)
	{
		return -1;
	}

	if (in_is_file(io))
	{
		in_close_file(io);
	}
	else if (in_is_buffer(io))
	{
		io->in_buffer = NULL;

		io->in_size = 0;
		io->in_position = 0;

		io->in_func = NULL;
	}
	else
	{
		io->in_func = NULL;
		io->in_user_func = NULL;
	}
	
	return 0;
}


int out_set_file(universal_io *const io, const char *const path)
{
	return 0;
}

int out_set_buffer(universal_io *const io, const size_t size)
{
	return 0;
}

int out_set_func(universal_io *const io, const io_user_func func)
{
	if (out_clear(io))
	{
		return -1;
	}
	
	io->out_user_func = func;
	io->out_func = &out_func_user;

	return 0;
}


int out_is_correct(const universal_io *const io)
{
	return io != NULL && (out_is_file(io) || out_is_buffer(io) || out_is_func(io));;
}

int out_is_file(const universal_io *const io)
{
	return io != NULL && io->out_file != NULL;
}

int out_is_buffer(const universal_io *const io)
{
	return io != NULL && io->out_buffer != NULL;
}

int out_is_func(const universal_io *const io)
{
	return io != NULL && io->out_user_func != NULL;
}


io_func out_get_func(const universal_io *const io)
{
	return io != NULL ? io->out_func : NULL;
}

size_t out_get_path(const universal_io *const io, char *const buffer)
{
	return 0;
}


char *out_extract_buffer(universal_io *const io)
{
	if (!out_is_buffer(io))
	{
		return NULL;
	}

	char *buffer = io->out_buffer;
	io->out_buffer = NULL;

	io->out_size = 0;
	io->out_position = 0;

	io->out_func = NULL;
	return buffer;
}

int out_close_file(universal_io *const io)
{
	return 0;
}

int out_clear(universal_io *const io)
{
	if (io == NULL)
	{
		return -1;
	}

	if (out_is_file(io))
	{
		out_close_file(io);
	}
	else if (out_is_buffer(io))
	{
		free(out_extract_buffer(io));
	}
	else
	{
		io->out_func = NULL;
		io->out_user_func = NULL;
	}
	
	return 0;
}


int io_erase(universal_io *const io)
{
	return in_clear(io) || out_clear(io) ? -1 : 0;
}
