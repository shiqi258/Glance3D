/**
 * @class   vtkF3DImguiConsole
 * @brief   An ImGui console window
 *
 * This class is used instead of vtkF3DConsoleOutputWindow if F3D_MODULE_UI is enabled
 * On top of the regular behavior of printing the log in the console, all the logs are also added
 * in an imgui window so the user can access it easily by calling ShowConsole()
 * It is also adding an input widget where commands registered in libf3d can be executed.
 * Finally, a small icon is displayed on the top right corner when the console is hidden but a new
 * warning or error is logged.
 */

#ifndef vtkF3DImguiConsole_h
#define vtkF3DImguiConsole_h

#include "vtkF3DConsoleOutputWindow.h"

#include <vtkCommand.h>

#include <functional>
#include <memory>

class vtkOpenGLRenderWindow;
class vtkWindow;
struct ImVec2;

class vtkF3DImguiConsole : public vtkF3DConsoleOutputWindow
{
public:
  static vtkF3DImguiConsole* New();
  vtkTypeMacro(vtkF3DImguiConsole, vtkF3DConsoleOutputWindow);

  /**
   * Add text to console
   */
  void DisplayText(const char*) override;

  /**
   * Show the console: the command palette (top-centered overlay with live suggestions and the
   * recent log tail) or, when @p minimal, the single input line. @p topOffset keeps the window
   * clear of the docked top bar when the panel chrome is open. @p rightInset is the width the
   * minimal line must leave free at the right end for the viewport's top-right chrome column --
   * the caller reads it from the single owner of that corner rather than measuring whatever
   * happens to be up there.
   */
  void ShowConsole(bool minimal, float topOffset = 0.f, float rightInset = 0.f);

  /**
   * Clear the console log.
   *
   * Deliberately does NOT clear the message history: `clear` is a user-visible command about the
   * console's own log tail, and the message center is a different surface with a different
   * lifetime. It does mark the messages read, because reading the console is how a user learns
   * what happened -- the unread bell would otherwise keep pointing at what they just read.
   */
  void Clear();

  /**
   * Set the callback to get completion candidates
   */
  void SetCompletionCallback(
    std::function<std::vector<std::string>(const std::string& pattern)> callback);

protected:
  vtkF3DImguiConsole();
  ~vtkF3DImguiConsole() override;

private:
  struct Internals;
  std::unique_ptr<Internals> Pimpl;

private:
  vtkF3DImguiConsole(const vtkF3DImguiConsole&) = delete;
  void operator=(const vtkF3DImguiConsole&) = delete;
};

#endif
