#include "G3DCommandLine.h"

//----------------------------------------------------------------------------
std::string G3DQuoteCommandArgument(std::string_view arg)
{
  std::string quoted;
  quoted.reserve(arg.size() + 2);
  quoted.push_back('"');
  for (const char c : arg)
  {
    // The only two characters tokenize treats specially inside double quotes.
    if (c == '\\' || c == '"')
    {
      quoted.push_back('\\');
    }
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
}

//----------------------------------------------------------------------------
std::string G3DCommandLine(std::string_view name, std::initializer_list<std::string_view> args)
{
  std::string line(name);
  for (const std::string_view arg : args)
  {
    line.push_back(' ');
    line += G3DQuoteCommandArgument(arg);
  }
  return line;
}
