require "tundra.syntax.glob"
require "src.msgcompiler"

StaticLibrary {
	Name = "common",
	Sources =  {
		"src/util.c", "src/transport.c", "src/peer.c", "src/protocol.c", "src/socket_includes.c",
		CompileNetMessages {
			Pass = "Codegen",
			Input = 'src/rlnet.msg',
			OutputStem = 'rlnet',
		}
	},
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
}


Program {
	Config = { "macosx-*-*", "win64-*-*" },
	Name = "rl-controller",
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
	Sources = {
		"src/controller.c", "src/file_server.c"
	},
	Depends = {
		"common"
	},
  Libs = {
    { "ws2_32.lib"; Config = "win64-*-*" },
  },
}

Program {
	Name = "rl-target",
	Libs = { "amiga"; Config = "amiga-*-*" },
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
	Sources = {
    { "src/amigafs.c"; Config = "amiga-*-*" },
    "src/target.c"
	},
	Depends = {
		"common"
	},
  Libs = {
    { "ws2_32.lib"; Config = "win64-*-*" },
  },
}

Default "rl-controller"
Default "rl-target"
