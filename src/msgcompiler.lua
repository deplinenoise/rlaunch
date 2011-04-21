module(..., package.seeall)

function apply(decl_parser, passes)
	decl_parser:add_source_generator("CompileNetMessages", function (args)
		local passname = assert(args.Pass, "no pass specified")
		local pass = passes[passname]
		if not pass then
			errorf("can't find pass %s", passname)
		end
		local input = assert(args.Input, "no source file specified!")
		local stem = assert(args.OutputStem, "no output stem specified!")
		local fnstem = "$(OBJECTDIR)/_generated/" .. stem
		local header = fnstem .. ".h"
		local source = fnstem .. ".c"
		return function (env)
			return env:make_node {
				Label = "CompileNetMessages $(@)",
				Action = "python src/mkmsg.py " .. fnstem .. " < $(<)",
				Pass = pass,
				InputFiles = { input },
				OutputFiles = { header, source },
				ImplicitInputs = { "src/mkmsg.py" },
			}
		end
	end)
end

