/**
 * @class   F3DLog
 * @brief   Namespace containing methods to print logs
 *
 * Provide simple utilities to print output visible to the user.
 * This should be used with vtkF3DObjectFactory from applicative module
 * in order to have the expected behavior using F3D output message windows.
 *
 */

#ifndef F3DLog_h
#define F3DLog_h

#include <cstdint>
#include <functional>
#include <string>

namespace F3DLog
{
enum class Severity : unsigned char
{
  Debug = 0,
  Info,
  Warning,
  Error,
  Quiet
};

enum class StandardStream : unsigned char
{
  Default = 0,
  None,
  AlwaysStdErr
};

/**
 * Set this global variable to control the verbose level
 * that actually display something in Print
 */
extern Severity VerboseLevel;

/**
 * Print a message with corresponding severity in the output window
 */
void Print(Severity sev, const std::string& msg);

/**
 * If output window is a vtkF3DConsoleOutputWindow,
 * set the coloring usage.
 */
void SetUseColoring(bool use);

/**
 * Determine how standard stream should be used.
 * If mode is None, then no message is written at all (including errors).
 * If mode is AlwaysStdErr, then all messages are written to stderr.
 * If mode is Default, then only warnings and errors are written to stderr. Debug and info messages
 * are written to stdout.
 */
void SetStandardStream(StandardStream mode);

/**
 * Subscribe to every log message. Returns a token for RemoveForwarder().
 *
 * Multicast on purpose: the session file log, the exported libf3d facade and the wasm bindings all
 * want the stream, and the single-slot version meant whichever registered last silently switched
 * the others off -- which is exactly how a JS `Log.forward` call used to kill the file log.
 *
 * Forwarders are called OUTSIDE the registry lock, from the thread that logged. A forwarder that
 * itself logs would otherwise deadlock.
 */
std::uint64_t AddForwarder(std::function<void(Severity, const std::string&)> callback);

/// Unsubscribe a forwarder added by AddForwarder(). Unknown tokens are ignored.
void RemoveForwarder(std::uint64_t token);

/**
 * Set THE legacy single forwarder (null clears it).
 *
 * Kept because it is what f3d::log::forward() exposes, and because F3DLogFile's destructor calls
 * forward(nullptr) on the way out -- with a naive multicast that would have unsubscribed everyone
 * else too. It owns one reserved slot and touches nothing added through AddForwarder().
 */
void Forward(std::function<void(Severity, const std::string&)> userCallback);
};

#endif
