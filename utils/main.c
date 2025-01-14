#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utils/tools.h>


#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

int main(int argc, char* argv[])
{
	struct tr_cmd cmds[] = {
		tr_create,
		tr_edit,
		tr_remote,
		tr_show,
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		if (strcmp(argv[1], cmds[i].name) == 0)
			return cmds[i].cmd(argc - 1, ++argv);
	}

	return -1;
}
