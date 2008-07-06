
rlaunch -- A remote launcher for Amiga development

PURPOSE

The rlaunch utility allows the user to develop programs using the various
cross-compilers available and then test them immediately on an Amiga computer.

THEORY OF OPERATION

There are two parts to rlaunch, the controller and the target. The target
binary runs as a daemon on the Amiga and sets up a TCP/IP port (7001 by
default) listening for incoming connections. The controller runs on some other
machine (UNIX or Win32) and is started on demand by the user.

When the controller is started, it is given at least two options, the path to
the file tree to present to the Amiga, and the path to an Amiga executable to
be started. The controller initiates a connection to the target and instructs
it to launch said executable.

The target process knows how to set up a virtual file system on the Amiga for
each controller connection which is also used to load the designated
executable. These devices (typically named RL0, RL1 and so on) are maintained
automatically by the target process for the lifetime of spawned executables.
Requests against these virtual file system devices will map back over the same
TCP/IP connection and operate on the controller's file system.

For example; if a controller is started on Win32 with the file serving path C:\Foo
and the executable Bar, the following events will take place (in order):

1. A connection is made to the specified target
2. A "launch executable" request is transmitted from controller to target
3. The target creates a new virtual file system RLx:, mapping it to this controller
4. The target creates the process "RLx:Bar"
5. When the connection is broken or the target program is interrupted, the device is torn down.
