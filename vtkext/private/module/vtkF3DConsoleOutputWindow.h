/**
 * @class   vtkF3DConsoleOutputWindow
 * @brief   Custom console output window
 *
 */
#ifndef vtkF3DConsoleOutputWindow_h
#define vtkF3DConsoleOutputWindow_h

#include <vtkOutputWindow.h>

#include <vtkCommand.h>

class vtkF3DConsoleOutputWindow : public vtkOutputWindow
{
public:
  vtkTypeMacro(vtkF3DConsoleOutputWindow, vtkOutputWindow);
  static vtkF3DConsoleOutputWindow* New();

  /**
   * Reimplemented to support coloring
   */
  void DisplayText(const char*) override;

  //@{
  /**
   * Set/Get the coloring usage.
   * Default is true.
   */
  vtkSetMacro(UseColoring, bool);
  vtkGetMacro(UseColoring, bool);
  //@}

  vtkF3DConsoleOutputWindow(const vtkF3DConsoleOutputWindow&) = delete;
  void operator=(const vtkF3DConsoleOutputWindow&) = delete;

protected:
  vtkF3DConsoleOutputWindow();
  ~vtkF3DConsoleOutputWindow() override = default;

  /**
   * Feed @p txt to the notification center, classified from the current VTK message type. Called
   * at the top of DisplayText so both message streams (libf3d via F3DLog, and VTK internals via
   * the macros) are captured exactly once, in every build including the UI-less wasm one.
   */
  void NotifyCenterCapture(const char* txt);

private:
  bool UseColoring = true;
};

#endif
