#ifndef RLAUNCH_VERSION_H
#define RLAUNCH_VERSION_H

#define RLAUNCH_BASE_DEVICE_NAME "TBL"
#define RLAUNCH_VIRTUAL_INPUT_FILE "+virtual-input+"
#define RLAUNCH_VIRTUAL_OUTPUT_FILE "+virtual-output+"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define RLAUNCH_VER_MAJOR 1
/* NB: Interpreted as octal in the code.. */
#define RLAUNCH_VER_MINOR 0

#define RLAUNCH_VER_MAJOR_STR TOSTRING(RLAUNCH_VER_MAJOR)
#define RLAUNCH_VER_MINOR_STR TOSTRING(RLAUNCH_VER_MINOR)

#define RLAUNCH_VERSION RLAUNCH_VER_MAJOR_STR "." RLAUNCH_VER_MINOR_STR

#define RLAUNCH_LICENSE "Copyright (c)2009 Andreas Fredriksson, TBL Technologies. All rights reserved."

#endif
