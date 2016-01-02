/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <math.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config-parser.h"
#include "gcode-machine-control.h"
#include "gcode-parser.h"
#include "logging.h"
#include "motion-queue.h"
#include "motor-operations.h"
#include "sim-firmware.h"
#include "string-util.h"

static int usage(const char *prog, const char *msg) {
  if (msg) {
    fprintf(stderr, "\033[1m\033[31m%s\033[0m\n\n", msg);
  }
  fprintf(stderr, "Usage: %s [options] [<gcode-filename>]\n"
	  "Options:\n", prog);
  fprintf(stderr,
          "  --config <config>     (-c): Configuration file.\n"
	  "  --port <port>         (-p): Listen on this TCP port for GCode.\n"
	  "  --bind-addr <bind-ip> (-b): Bind to this IP (Default: 0.0.0.0).\n"
          "  --logfile <logfile>   (-l): Logfile to use. If empty, messages go to syslog (Default: /dev/stderr).\n"
          "  --daemon              (-d): Run as daemon.\n"
          "  --priv <uid>[:<gid>]      : After opening GPIO: drop privileges to this (default: daemon:daemon)\n"
          "\nMostly for testing and debugging:\n"
	  "  -f <factor>               : Feedrate speed factor (Default 1.0).\n"
	  "  -n                        : Dryrun; don't send to motors, no GPIO or PRU needed (Default: off).\n"
          // -N dry-run with simulation output; mostly for development, so not mentioned here.
	  "  -P                        : Verbose: Show some more debug output (Default: off).\n"
	  "  -S                        : Synchronous: don't queue (Default: off).\n"
	  "  --loop[=count]            : Loop file number of times (no value: forever; equal sign with value important.)\n");
  return 1;
}

static int fyi_option_gone() {
  fprintf(stderr,
          "Options for machine settings have been removed in favor of a configuration file.\n"
          "Provide it with -c <config-file>.\n"
          "See https://github.com/hzeller/beagleg/blob/master/sample.config\n");
  return 1;
}

// Call with <uid>:<gid> string.
static bool drop_privileges(StringPiece privs) {
  std::vector<StringPiece> pair = SplitString(privs, ":");
  if (pair.size() < 1 || pair.size() > 2) {
    Log_error("Drop privileges: Require colon separated <uid>[:<gid>]");
    return false;
  }

  std::string name;
  if (pair.size() == 2) {
    name = pair[1].ToString();
    struct group *g = getgrnam(name.c_str());
    if (g == NULL) {
      Log_error("Drop privileges: Couldn't look up group '%s'.", name.c_str());
      return false;
    }
    if (setresgid(g->gr_gid, g->gr_gid, g->gr_gid) != 0) {
      Log_error("Couldn't drop group privs to '%s' (gid %d): %s",
                name.c_str(), g->gr_gid, strerror(errno));
      return false;
    }
  }

  name = pair[0].ToString();
  struct passwd *p = getpwnam(name.c_str());
  if (p == NULL) {
    Log_error("Drop privileges: Couldn't look up user '%s'.", name.c_str());
    return false;
  }
  if (setresuid(p->pw_uid, p->pw_uid, p->pw_uid) != 0) {
    Log_error("Couldn't drop user privs to '%s' (uid %d): %s",
              name.c_str(), p->pw_uid, strerror(errno));
    return false;
  }

  return true;
}

// Reads the given "gcode_filename" with GCode and operates machine with it.
// If "loop_count" is >= 0, repeats this number after the first execution.
static int send_file_to_machine(GCodeMachineControl *machine,
                                GCodeParser *parser,
                                const char *gcode_filename, int loop_count) {
  int ret;
  machine->SetMsgOut(stderr);
  while (loop_count < 0 || loop_count-- > 0) {
    int fd = open(gcode_filename, O_RDONLY);
    ret = parser->ParseStream(fd, stderr);
    if (ret != 0)
      break;
  }
  return ret;
}

// Open server. Return file-descriptor or -1 if listen fails.
// Bind to "bind_addr" (can be NULL, then it is 0.0.0.0) and "port".
static int open_server(const char *bind_addr, int port) {
  if (port > 65535) {
    Log_error("Invalid port %d\n", port);
    return -1;
  }
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    Log_error("creating socket: %s", strerror(errno));
    return -1;
  }

  struct sockaddr_in serv_addr = {0};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  if (bind_addr && !inet_pton(AF_INET, bind_addr, &serv_addr.sin_addr.s_addr)) {
    Log_error("Invalid bind IP address %s\n", bind_addr);
    return -1;
  }
  serv_addr.sin_port = htons(port);
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(s, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    Log_error("Trouble binding to %s:%d: %s",
              bind_addr ? bind_addr : "0.0.0.0", port,
              strerror(errno));
    return -1;
  }
  return s;
}

// Accept connections and receive GCode.
// Only one connection can be active at a time.
// Socket must already be opened by open_server(). "bind_addr" and "port"
// are just FYI information for nicer log-messages.
static int run_server(int listen_socket,
                      GCodeMachineControl *machine, GCodeParser *parser,
                      const char *bind_addr, int port) {
  signal(SIGPIPE, SIG_IGN);  // Pesky clients, closing connections...

  if (listen(listen_socket, 2) < 0) {
    Log_error("listen() failed: %s", strerror(errno));
    return 1;
  }

  Log_info("Ready to accept GCode-connections on %s:%d",
           bind_addr ? bind_addr : "0.0.0.0", port);

  int process_result;
  do {
    struct sockaddr_in client;
    socklen_t socklen = sizeof(client);
    int connection = accept(listen_socket, (struct sockaddr*) &client, &socklen);
    if (connection < 0) {
      Log_error("accept(): %s", strerror(errno));
      return 1;
    }
    char ip_buffer[INET_ADDRSTRLEN];
    const char *print_ip = inet_ntop(AF_INET, &client.sin_addr,
				     ip_buffer, sizeof(ip_buffer));
    Log_info("Accepting new connection from %s\n", print_ip);
    FILE *msg_stream = fdopen(connection, "w");
    machine->SetMsgOut(msg_stream);
    process_result = parser->ParseStream(connection, msg_stream);

    fclose(msg_stream);
    Log_info("Connection to %s closed.\n", print_ip);
  } while (process_result == 0);

  close(listen_socket);
  Log_error("Error gcode_machine_control_from_stream() == %d. Exiting\n",
            process_result);

  return process_result;
}

int main(int argc, char *argv[]) {
  MachineControlConfig config;
  bool dry_run = false;
  bool simulation_output = false;
  const char *logfile = NULL;
  const char *config_file = NULL;
  bool as_daemon = false;
  const char *privs = "daemon:daemon";

  // Less common options don't have a short option.
  enum LongOptionsOnly {
    OPT_SET_STEPS_MM = 1000,
    OPT_SET_HOME_POS,
    OPT_SET_HOME_ORDER,
    OPT_SET_MOTOR_MAPPING,
    OPT_SET_MIN_ENDSWITCH,
    OPT_SET_MAX_ENDSWITCH,
    OPT_SET_ENDSWITCH_POLARITY,
    OPT_SET_THRESHOLD_ANGLE,
    OPT_REQUIRE_HOMING,
    OPT_DISABLE_RANGE_CHECK,
    OPT_LOOP,
    OPT_PRIVS,
  };

  static struct option long_options[] = {
    { "config",             required_argument, NULL, 'c'},
    { "homing-required",    no_argument,       NULL, OPT_REQUIRE_HOMING },
    { "disable-range-check",no_argument,       NULL, OPT_DISABLE_RANGE_CHECK },

    { "port",               required_argument, NULL, 'p'},
    { "bind-addr",          required_argument, NULL, 'b'},
    { "loop",               optional_argument, NULL, OPT_LOOP },
    { "logfile",            required_argument, NULL, 'l'},
    { "daemon",             no_argument,       NULL, 'd'},
    { "priv",               required_argument, NULL, OPT_PRIVS },

    // possibly deprecated soon.
    { "home-order",         required_argument, NULL, OPT_SET_HOME_ORDER },
    { "threshold-angle",    required_argument, NULL, OPT_SET_THRESHOLD_ANGLE },

    // deprecated.
    { "max-feedrate",       required_argument, NULL, 'm'},
    { "accel",              required_argument, NULL, 'a'},
    { "range",              required_argument, NULL, 'r' },
    { "steps-mm",           required_argument, NULL, OPT_SET_STEPS_MM },

    { "axis-mapping",       required_argument, NULL, OPT_SET_MOTOR_MAPPING },
    { "min-endswitch",      required_argument, NULL, OPT_SET_MIN_ENDSWITCH },
    { "max-endswitch",      required_argument, NULL, OPT_SET_MAX_ENDSWITCH },
    { "endswitch-polarity", required_argument, NULL, OPT_SET_ENDSWITCH_POLARITY },

    { 0,                    0,                 0,    0  },
  };

  int listen_port = -1;
  int file_loop_count = 1;
  char *bind_addr = NULL;
  int opt;
  while ((opt = getopt_long(argc, argv, "m:a:p:b:r:SPnNf:l:dc:",
			    long_options, NULL)) != -1) {
    switch (opt) {
    case 'f':
      config.speed_factor = (float)atof(optarg);
      if (config.speed_factor <= 0)
	return usage(argv[0], "Speedfactor cannot be <= 0");
      break;
    case OPT_SET_THRESHOLD_ANGLE:
      config.threshold_angle = (float)atof(optarg);
      break;
    case OPT_SET_HOME_ORDER:
      config.home_order = optarg;
      break;
    case OPT_REQUIRE_HOMING:
      config.require_homing = true;
      break;
    case OPT_DISABLE_RANGE_CHECK:
      config.range_check = false;
      break;
    case 'n':
      dry_run = true;
      break;
    case 'N':
      dry_run = true;
      simulation_output = true;
      break;
    case 'P':
      config.debug_print = true;
      break;
    case 'S':
      config.synchronous = true;
      break;
    case OPT_LOOP:
      file_loop_count = (optarg) ? atoi(optarg) : -1;
      break;
    case 'p':
      listen_port = atoi(optarg);
      break;
    case 'b':
      bind_addr = strdup(optarg);
      break;
    case 'l':
      logfile = strdup(optarg);
      break;
    case 'd':
      as_daemon = true;
      break;
    case 'c':
      config_file = strdup(optarg);
      break;
    case OPT_PRIVS:
      privs = strdup(optarg);
      break;

      // Deprecated options.
    case 'm':
    case 'a':
    case OPT_SET_STEPS_MM:
    case OPT_SET_MOTOR_MAPPING:
    case OPT_SET_MIN_ENDSWITCH:
    case OPT_SET_MAX_ENDSWITCH:
    case OPT_SET_ENDSWITCH_POLARITY:
    case 'r':  // range.
      return fyi_option_gone();

    default:
      return usage(argv[0], "Unknown flag");
    }
  }

  const bool has_filename = (optind < argc);
  if (! (has_filename ^ (listen_port > 0))) {
    return usage(argv[0], "Choose one: <gcode-filename> or --port <port>.");
  }
  if (!has_filename && file_loop_count != 1) {
    return usage(argv[0], "--loop only makes sense with a filename.");
  }

  // As daemon, we use whatever the use chose as logfile
  // (including nothing->syslog). Interactive, nothing means stderr.
  Log_init(as_daemon ? logfile : (logfile == NULL ? "/dev/stderr" : logfile));
  Log_info("Startup.");

  // If reading from file: don't print 'ok' for every line.
  config.acknowledge_lines = !has_filename;

  if (!config_file) {
    Log_error("Expected config file -c <config>");
    return 1;
  }

  ConfigParser config_parser;
  if (!config_parser.SetContentFromFile(config_file)) {
    Log_error("Exiting. Cannot read config file '%s'", config_file);
    return 1;
  }
  if (!config.InitializeFromFile(&config_parser)) {
    Log_error("Exiting. Parse error in configuration file '%s'", config_file);
    return 1;
  }
  // ... other configurations that read from that file.

  if (as_daemon) {
    if (daemon(0, 0) != 0) {
      Log_error("Can't become daemon: %s", strerror(errno));
    }
  }

  // Open socket early, so that we
  //  (a) can bail out early before messing with GPIO/PRU settings if
  //      someone is alrady listening (starting as daemon twice?).
  //  (b) open socket while we have not dropped privileges yet.
  int listen_socket = -1;
  if (!has_filename) {
    listen_socket = open_server(bind_addr, listen_port);
    if (listen_socket < 0) {
      Log_error("Exiting. Couldn't bind to socket to listen.");
      return 1;
    }
  }

  // The backend for our stepmotor control. We either talk to the PRU or
  // just ignore them on dummy.
  MotionQueue *motion_backend;
  if (dry_run) {
    // For dry-run, we never see switches, so disable them.
    for (int i = 0; i < GCODE_NUM_AXES; ++i) {
      config.min_endstop_[i].endstop_switch = 0;
      config.max_endstop_[i].endstop_switch = 0;
    }
    config.require_homing = false;

    // The backend
    if (simulation_output) {
      motion_backend = new SimFirmwareQueue(stdout, config.axis_mapping.length());
    } else {
      motion_backend = new DummyMotionQueue();
    }
  } else {
    if (geteuid() != 0) {
      Log_error("Exiting. Need to run as root to access GPIO pins "
                "(use the dryrun option -n to not write to GPIO).");
      Log_error("If you start as root, privileges will be dropped to '%s' "
                "after opening GPIO", privs);
      return 1;
    }
    motion_backend = new PRUMotionQueue();
  }

  // Listen port bound, GPIO initialized. Ready to drop privileges.
  if (geteuid() == 0 && strlen(privs) > 0) {
    if (drop_privileges(privs)) {
      Log_info("Dropped privileges to '%s'", privs);
    } else {
      Log_error("Exiting. Could not drop privileges to %s", privs);
      return 1;
    }
  }

  MotionQueueMotorOperations motor_operations(motion_backend);

  GCodeMachineControl *machine_control
    = GCodeMachineControl::Create(config, &motor_operations, stderr);
  if (machine_control == NULL) {
    Log_error("Exiting. Cannot initialize machine control.");
    return 1;
  }
  GCodeParser::Config parser_cfg;
  machine_control->GetHomePos(&parser_cfg.machine_origin);
  GCodeParser *parser = new GCodeParser(parser_cfg,
                                        machine_control->ParseEventReceiver());

  int ret = 0;
  if (has_filename) {
    const char *filename = argv[optind];
    ret = send_file_to_machine(machine_control, parser,
                               filename, file_loop_count);
  } else {
    ret = run_server(listen_socket, machine_control, parser,
                     bind_addr, listen_port);
  }

  delete parser;
  delete machine_control;

  const bool caught_signal = (ret == 2);
  if (caught_signal) {
    Log_info("Caught signal: immediate exit. "
             "Skipping potential remaining queue.");
  }
  motion_backend->Shutdown(!caught_signal);

  delete motion_backend;

  free(bind_addr);

  Log_info("Shutdown.");
  return ret;
}
