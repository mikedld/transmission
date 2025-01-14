struct tr_cmd {
	const char *name;
	int (*cmd)(int argc, char *argv[]);
};

extern struct tr_cmd tr_create;
extern struct tr_cmd tr_edit;
extern struct tr_cmd tr_remote;
extern struct tr_cmd tr_show;
