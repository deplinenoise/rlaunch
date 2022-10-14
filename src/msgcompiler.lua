module(..., package.seeall)

DefRule {
  Name = "CompileNetMessages",
  Command = "python3 src/mkmsg.py $(MSGSTEM) < $(<)",
  ImplicitInputs = { "src/mkmsg.py" },
  Blueprint = {
    Input = { Type = "string", Required = true },
    OutputStem = { Type = "string", Required = true },
  },
  Setup = function (env, data)
    local stem = "$(OBJECTDIR)/" .. data.OutputStem
    env:set("MSGSTEM", stem)
    return {
      InputFiles = { data.Input },
      OutputFiles = {
        stem .. '.h',
        stem .. '.c',
      },
    }
  end,
}
