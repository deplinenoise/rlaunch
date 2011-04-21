
local CFiles = { ".c", ".h" }

local common = {
	Defines = {
		{ "NDEBUG"; Config = "*-*-release" },
	},
}

local osx = {
	Inherit = common,
	Env = {
		CCOPTS = {
			"-Wall", "-Werror", "-g",
			{ "-O2"; Config = "*-*-release" },
		},
	},
}

local amiga = {
	Inherit = common,
	Env = {
		CPPDEFS = { "NO_C_LIB" },
		CCOPTS = {
			"-warn=-1",
			"-dontwarn=163", "-dontwarn=307", "-dontwarn=65",
			"-dontwarn=166", "-dontwarn=167", "-dontwarn=81"
		},
	},
}

Build {
	Passes = {
		Codegen = { Name = "Generate Code", BuildOrder = 1 },
	},
	Configs = {
		Config { Name = "linux-gcc", DefaultOnHost = "linux", Tools = { "gcc" }, },
		Config { Name = "macosx-gcc", DefaultOnHost = "macosx", Tools = { "gcc-osx" }, Inherit = osx },
		Config { Name = "win64-msvc", DefaultOnHost = "windows", Tools = { "msvc-vs2008"; TargetPlatform = "x64" }, },
		Config { Name = "amiga-vbcc", Inherit = amiga, Tools = { "vbcc" }, },
	},
	Units = "units.lua",
	ScriptDirs = { "." },
	SyntaxExtensions = { "tundra.syntax.glob", "src.msgcompiler" },
}
