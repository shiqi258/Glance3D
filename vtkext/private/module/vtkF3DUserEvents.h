/**
 * @class   vtkF3DUserEvents
 * @brief   An enum of F3D defined VTK events
 */

#ifndef vtkF3DUserEvents_h
#define vtkF3DUserEvents_h

#include <vtkCommand.h>

/**
 * Custom events
 */
enum vtkF3DUserEvents
{
  DropFilesEvent = vtkCommand::UserEvent + 100,
  KeyPressEvent,
  TriggerEvent,
  ShowEvent,
  HideEvent,
  SceneHierarchyChangedEvent,
  TraceEvent ///< calldata = const char* message, routed to f3d::log::debug (observation logging
             ///< from VTK-side modules that cannot link libf3d; lands in the session log file)
};

#endif
