
rlaunch -- A remote command launcher and file system driver for Amiga development

PURPOSE

The rlaunch utility allows the user to develop programs using the various
cross-compilers available and then test them immediately on an Amiga computer
without copying files.

Win32 & POSIX host command:

  rl-controller [-fsroot <directory>] [-port <port#>] [-log [a0][dniwc..]] <hostname> <path/to/executable>

   <hostname>             Hostname to connect to (mandatory)
   <path/to/executable>   Path to executable relative to fsroot, with forward slashes. (mandatory)

   Optional parameters:
   -fsroot                Specifies the file serving directory. The executable
                          must live in this directory as well. (default: cwd)
   -port                  The TCP port to connect to on the remote target (default: 7001)
   -log                   Specifies log levels
                          0: disable everything (default)
                          a: enable everything
                          d: enable debug channel
                          i: enable info channel
                          w: enable warning channel
                          c: enable console channel

Amiga target command:

  rl-target ADDRESS,PORT/N,LOG

  ADDRESS          IP address to bind, defaults to 0.0.0.0
  PORT/N           TCP port to bind, defaults to 7001
  LOG              Specifes log levels, see above.

THEORY OF OPERATION

There are two parts to rlaunch, the controller and the target. The target
binary runs as a daemon on the Amiga and sets up a TCP/IP port (7001 by
default) listening for incoming connections. The controller runs on some other
machine (UNIX or Win32) and is started on demand by the user when a program is
to be launched on the Amiga.

When the controller is started, it is given at least two options, the path to
the file tree to present to the Amiga, and the path to an Amiga executable to
be started. The controller initiates a connection to the target and instructs
it to launch said executable.

The target process knows how to set up a virtual file system on the Amiga for
each controller connection which is also used to load the designated
executable. These devices (named TBL0, TBL1 and so on) are maintained
automatically by the target process for the lifetime of controller connection.
Requests against these virtual file system devices will map back over the same
TCP/IP connection and operate on the controller's file system.

For example; if a controller is started on Win32 with the file serving path C:\Foo
and the executable Bar, the following events will take place (in order):

1. A connection is made to the specified target
2. A "launch executable" request is transmitted from controller to target
3. The target creates a new virtual file system TBLx:, mapping it to this controller
4. The target creates the process "TBLx:Bar"
5. When the connection is broken or the target program is interrupted, the device is torn down.

BUGS AND LIMITATIONS:

- All paths are limited to 108 characters. They will be silently truncated.
- Don't try to launch executables too quickly, the async launcher relies on static storage.
- The target doesn't currently terminate the connection when the started program dies.
- The target doesn't support more than one device (TBL0 is always used).
