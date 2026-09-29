#include "PseudoUnitTest.h"

#include <g3dCommand.h>
#include <utils.h>

#include <string>
#include <vector>

// g3d::command has to be the exact inverse of f3d::utils::tokenize: a value spliced into a command
// (a path in a notification action, an array name from the inspector, a dropped file) must come back
// out of the tokenizer unchanged. Before it existed, the path in the "Load it anyway" and "Open
// containing folder" actions lost every backslash on the way.
int TestSDKG3DCommand([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
  PseudoUnitTest test;

  const std::vector<std::string> values = {
    R"(G:\developmentProjects\Glance3D\testing\data\WaterBottle.glb)",
    R"(C:\Program Files\Some Dir\model name.stl)",
    R"(\\server\share\file.gltf)",
    R"(ends with a backslash\)",
    R"(a "quoted" name)",
    R"(single ' and back ` quotes)",
    "#not a comment",
    // "中文\模型.gltf" and a path copied from Explorer's properties, which starts with U+202A.
    "\xE4\xB8\xAD\xE6\x96\x87\\\xE6\xA8\xA1\xE5\x9E\x8B.gltf",
    "\xE2\x80\xAA"
    "G:\\a path copied with a bidi mark.glb",
    "plain",
  };
  for (const std::string& value : values)
  {
    test("round trip of " + value, f3d::utils::tokenize(g3d::command::line("cmd", { value })),
      std::vector<std::string>{ "cmd", value });
  }

  test("several arguments keep their boundaries",
    f3d::utils::tokenize(g3d::command::line("set", { "model.scivis.array_name", R"(a b\c)" })),
    std::vector<std::string>{ "set", "model.scivis.array_name", R"(a b\c)" });

  // What the tokenizer does to the raw splice this replaces.
  test("a raw splice loses the separators",
    f3d::utils::tokenize(std::string("cmd \"") + R"(G:\data\a.glb)" + "\""),
    std::vector<std::string>{ "cmd", "G:dataa.glb" });

  return test.result();
}
