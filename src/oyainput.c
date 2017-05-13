#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include<linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>

#include "common.h"
#include "oyainput.h"
#include "functions.h"
#include "config.h"
#include "oyastate.h"


static int do_terminate = 0;
static char devpath[BUFSIZE+1] = {};
static int fdo = 0;
static int paused = 0;
static int imtype = 0; // (0: none, 1:fcitx, 2:ibus, 3:uim)
static __u16 on_keycode = 0;
static __u16 off_keycode = 0;

int get_kbdevie_output() {
	return fdo;
}

int get_imtype() {
	return imtype;
}

void set_imtype(char* imname) {
	if(strcmp(imname, "fcitx")==0){
		imtype = 1;
	} else if(strcmp(imname, "ibus")==0){
		imtype = 2;
	} else if(strcmp(imname, "uim")==0){
		imtype = 3;
	} else {
		imtype = 0;
	}
}


void set_onkey(__u16 kc) {
	on_keycode = kc;
}

void set_offkey(__u16 kc){
	off_keycode = kc;
}

void set_inputdevice_path(char* new_devpath)
{
	printf("Keyboard Device Event File : %s\n", new_devpath);
	strncpy(devpath, new_devpath, BUFSIZE-1);
}

void on_term (int signal) {
	printf("\royainput terminated (SIG=%d)\n", signal);
	do_terminate = 1;
}

void set_signal_handler() {
	sigset_t mask;
	sigemptyset(&mask);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

void close_app(int fdi, int fdo) {
	ioctl(fdi, EVIOCGRAB, 0); // release hook of /dev/input/event0-9
	close(fdi);
	close(fdo);
	close_oya_state();
}

void create_user_input() {
	// global value: fdo
	fdo = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fdo == -1) {
		die("error: Failed to open uinput device event file! %s", "/dev/uinput");		
	}
	if(ioctl(fdo, UI_SET_EVBIT, EV_SYN) < 0) die("error: ioctl");
	if(ioctl(fdo, UI_SET_EVBIT, EV_KEY) < 0) die("error: ioctl");
	if(ioctl(fdo, UI_SET_EVBIT, EV_MSC) < 0) die("error: ioctl");
	for(int i = 0; i < KEY_MAX; ++i)
		if(ioctl(fdo, UI_SET_KEYBIT, i) < 0) die("error: ioctl");

	struct uinput_user_dev uidev;
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "oyainput");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor  = 0x1;
	uidev.id.product = 0x1;
	uidev.id.version = 1;

	if(write(fdo, &uidev, sizeof(uidev)) < 0) die("error: write");
	if(ioctl(fdo, UI_DEV_CREATE) < 0) die("error: ioctl");

}

int main(int argc, char *argv[]) {
	
	// set console bufferring mode off.
	setvbuf(stdout, (char*)NULL, _IONBF, 0);

	// check duplicate running
	if (exist_previous()) {
		die("error: oyainput is already running!");
	}

	uid_t euid = geteuid();
	if (euid != 0) {
		die("error: Only su(root) can run this program.");
	}
	
	// initialize oyayubi state by default values.
	oyayubi_state_init();

	// try search keyboard input device event file form /dev/input/event0-9
	find_kbdevent_path(devpath);

	char user_name[BUFSIZE+1] = {};
	strncpy(user_name, getenv("USER"), BUFSIZE);
	if (argc >= 2) {
		strncpy(user_name, argv[1], BUFSIZE);
	}
	struct passwd *pw = getpwnam(user_name);
	if (pw==NULL) {
		die("error: Invalid user name", user_name);
	}
	
	char confpath[BUFSIZE+1] = {};
	strncpy(confpath, pw->pw_dir, BUFSIZE);
	strcat(confpath, "/.oyainputconf");

	// load config file.
	if (! exist_file(confpath)) {
		save_config(confpath);
	}
	
	printf("Load config: %s\n", confpath);
	if(! load_config(confpath)) {
		die("error: Cannot load config file!\n");
	}

	if (get_imtype() == 1 &&
		0 != system("type fcitx-remote > /dev/null")) {
		die("error: Fcitx is not installed!");
	}
	
	create_infotables();

	int fdi = open(devpath, O_RDONLY);
	if (fdi == -1) {
		die("error: Failed to open keyboard device event file! %s", devpath);		
	}
	sleep(1); // DO NOT DELETE. need to intialize ioctl
	ioctl(fdi, EVIOCGRAB, 1); // start hook keyboard device

	create_user_input();
	set_signal_handler();

	// set userid to current user(this program must be run by root privilege)
	setuid(pw->pw_uid);
	seteuid(euid);

	fd_set rfds;
	struct timeval tv;
	int retval;
	Boolean ctrl_pressed  = FALSE;
	Boolean shift_pressed = FALSE;
	Boolean alt_pressed   = FALSE;
	struct input_event ie;
	OYAYUBI_EVENT oe;

	do_terminate = 0;
	printf("oyainput running...(CTRL+C to exit)\n");

	while(!do_terminate){

		FD_ZERO(&rfds);
		FD_SET(fdi, &rfds);

		tv.tv_sec  = 10;
		tv.tv_usec = 0;

		retval = select(fdi+1, &rfds, NULL, NULL, &tv);
		if(retval == -1 || retval == 0){
			continue;
		}
		
		memset(&ie, 0, sizeof(ie));
		if(read(fdi, &ie, sizeof(ie)) != sizeof(ie)) {
			close_app(fdi, fdo);
			exit( EXIT_FAILURE );
		}

		switch(ie.code) {
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
			ctrl_pressed = ((ie.value == 1)? TRUE : ((ie.value==0) ? FALSE : ctrl_pressed));
			write(fdo, &ie, sizeof(ie));
			break;
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
			shift_pressed = ((ie.value == 1)? TRUE : ((ie.value==0) ? FALSE : shift_pressed));
			write(fdo, &ie, sizeof(ie));
			break;
		case KEY_LEFTALT:
		//case KEY_RIGHTALT:
			alt_pressed = ((ie.value == 1)? TRUE : ((ie.value==0) ? FALSE : alt_pressed));
			write(fdo, &ie, sizeof(ie));
			break;
		case KEY_PAUSE:
			if (ie.value == 1) {
				if (paused) {
					printf("\royainput restarted.");
					paused = 0;
				} else {
					printf("\royainput paused.   ");
					paused = 1;
				}
			}
			write(fdo, &ie, sizeof(ie));
			break;
		default:

			if ( ie.type != EV_KEY) {
				write(fdo, &ie, sizeof(ie));
				break;
			}
		
			if (ie.value == 1) {
				if (paused && on_keycode != 0 && ie.code == on_keycode) {
					printf("\royainput restarted.");
					paused = 0;
					write(fdo, &ie, sizeof(ie));
					break;
				}
				if (! paused && off_keycode != 0 && ie.code == off_keycode) {
					printf("\royainput paused.   ");
					paused = 1;
					write(fdo, &ie, sizeof(ie));
					break;
				}
			}

			if (paused) {
				write(fdo, &ie, sizeof(ie));
				break;
			}

			if (shift_pressed || ctrl_pressed || alt_pressed) {
				write(fdo, &ie, sizeof(ie));
				break;
			}

			if (! is_acceptable(ie.code)) {
				write(fdo, &ie, sizeof(ie));
				break;
			}

			
			if (! is_imeon()) {
				write(fdo, &ie, sizeof(ie));
				break;
			}

			memset(&oe, 0, sizeof(oe));
			oe.eventType = ET_KEYDOWN;
			oe.isRepeat = 0;
			if (ie.value==0) {
				oe.eventType = ET_KEYUP;
			} else if (ie.value==1) {
			} else if (ie.value==2) {
				oe.isRepeat = 1;
			}
			oe.keyCode = ie.code;
			handle_oyayubi_event(oe);
			break;
		}
	}
 
	close_app(fdi,fdo);
	return EXIT_SUCCESS;
}




