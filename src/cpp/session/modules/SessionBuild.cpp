/*
 * SessionBuild.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * This program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionBuild.hpp"

#include <vector>

#include <boost/utility.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/format.hpp>
#include <boost/scope_exit.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/join.hpp>

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/text/DcfParser.hpp>
#include <core/system/Process.hpp>
#include <core/system/Environment.hpp>
#include <core/system/ShellUtils.hpp>
#include <core/r_util/RPackageInfo.hpp>


#ifdef _WIN32
#include <core/r_util/RToolsInfo.hpp>
#endif

#include <r/RExec.hpp>
#include <r/RRoutines.hpp>
#include <r/session/RSessionUtils.hpp>
#include <r/session/RConsoleHistory.hpp>

#include <session/projects/SessionProjects.hpp>
#include <session/SessionUserSettings.hpp>
#include <session/SessionModuleContext.hpp>

#include "SessionBuildErrors.hpp"

using namespace core;

namespace session {

namespace {

#ifdef _WIN32

void addRtoolsToPathIfNecessary(core::system::Options* pEnvironment)
{
    // can we find ls.exe and gcc.exe on the path? if so then
    // we assume Rtools are already there (this is the same test
    // used by devtools)
    bool rToolsOnPath = false;
    Error error = r::exec::RFunction(".rs.isRtoolsOnPath").call(&rToolsOnPath);
    if (error)
       LOG_ERROR(error);
    if (rToolsOnPath)
       return;

    // ok so scan for R tools
    std::vector<r_util::RToolsInfo> rTools;
    error = core::r_util::discoverRTools(&rTools);
    if (error)
    {
       LOG_ERROR(error);
       return;
    }

    // enumerate them to see if we have a compatible version
    // (go in reverse order for most recent first)
    std::vector<r_util::RToolsInfo>::const_reverse_iterator it = rTools.rbegin();
    for ( ; it != rTools.rend(); ++it)
    {
       bool isCompatible = false;
       error = r::exec::evaluateString(it->versionPredicate(), &isCompatible);
       if (isCompatible)
       {
          r_util::prependToSystemPath(*it, pEnvironment);
          return;
       }
    }
 }

#else

void addRtoolsToPathIfNecessary(core::system::Options* pEnvironment);

#endif


std::string libPathsString()
{
   // call into R to get the string
   std::string libPaths;
   Error error = r::exec::RFunction(".rs.libPathsString").call(&libPaths);
   if (error)
   {
      LOG_ERROR(error);
      return std::string();
   }

   // this is presumably system-encoded, so convert this to utf8 before return
   return string_utils::systemToUtf8(libPaths);
}

shell_utils::ShellCommand buildRCmd(const core::FilePath& rBinDir)
{
#if defined(_WIN32)
   shell_utils::ShellCommand rCmd(rBinDir.childPath("Rcmd.exe"));
#else
   shell_utils::ShellCommand rCmd(rBinDir.childPath("R"));
   rCmd << "CMD";
#endif
   return rCmd;
}

// R command invocation -- has two representations, one to be submitted
// (shellCmd_) and one to show the user (cmdString_)
class RCommand
{
public:
   explicit RCommand(const FilePath& rBinDir)
      : shellCmd_(buildRCmd(rBinDir))
   {
#ifdef _WIN32
      cmdString_ = "Rcmd.exe";
#else
      cmdString_ = "R CMD";
#endif

      // set escape mode to files-only. this is so that when we
      // add the group of extra arguments from the user that we
      // don't put quotes around it.
      shellCmd_ << shell_utils::EscapeFilesOnly;
   }

   RCommand& operator<<(const std::string& arg)
   {
      if (!arg.empty())
      {
         cmdString_ += " " + arg;
         shellCmd_ << arg;
      }
      return *this;
   }

   RCommand& operator<<(const FilePath& arg)
   {
      cmdString_ += " " + arg.absolutePath();
      shellCmd_ << arg;
      return *this;
   }


   const std::string& commandString() const
   {
      return cmdString_;
   }

   const shell_utils::ShellCommand& shellCommand() const
   {
      return shellCmd_;
   }

private:
   std::string cmdString_;
   shell_utils::ShellCommand shellCmd_;
};

} // anonymous namespace

namespace modules { 
namespace build {

namespace {

const char * const kRoxygenizePackage = "roxygenize-package";
const char * const kBuildSourcePackage = "build-source-package";
const char * const kBuildBinaryPackage = "build-binary-package";
const char * const kCheckPackage = "check-package";
const char * const kBuildAndReload = "build-all";
const char * const kRebuildAll = "rebuild-all";

class Build : boost::noncopyable,
              public boost::enable_shared_from_this<Build>
{
public:
   static boost::shared_ptr<Build> create(const std::string& type)
   {
      boost::shared_ptr<Build> pBuild(new Build());
      pBuild->start(type);
      return pBuild;
   }

private:
   Build()
      : isRunning_(false), terminationRequested_(false), restartR_(false)
   {
   }

   void start(const std::string& type)
   {
      ClientEvent event(client_events::kBuildStarted);
      module_context::enqueClientEvent(event);

      isRunning_ = true;

      // read build options
      Error error = projects::projectContext().readBuildOptions(&options_);
      if (error)
      {
         terminateWithError("reading build options file", error);
         return;
      }

      // callbacks
      core::system::ProcessCallbacks cb;
      cb.onContinue = boost::bind(&Build::onContinue,
                                  Build::shared_from_this());
      cb.onStdout = boost::bind(&Build::onOutput,
                                Build::shared_from_this(), _2);
      cb.onStderr = boost::bind(&Build::onOutput,
                                Build::shared_from_this(), _2);
      cb.onExit =  boost::bind(&Build::onCompleted,
                                Build::shared_from_this(),
                                _1);

      // execute build
      executeBuild(type, cb);
   }


   void executeBuild(const std::string& type,
                     const core::system::ProcessCallbacks& cb)
   {
      // options
      core::system::ProcessOptions options;
      options.terminateChildren = true;
      options.redirectStdErrToStdOut = true;

      FilePath buildTargetPath = projects::projectContext().buildTargetPath();
      const core::r_util::RProjectConfig& config = projectConfig();
      if (config.buildType == r_util::kBuildTypePackage)
      {
         options.workingDir = buildTargetPath.parent();
         executePackageBuild(type, buildTargetPath, options, cb);
      }
      else if (config.buildType == r_util::kBuildTypeMakefile)
      {
         options.workingDir = buildTargetPath;
         executeMakefileBuild(type, buildTargetPath, options, cb);
      }
      else if (config.buildType == r_util::kBuildTypeCustom)
      {
         options.workingDir = buildTargetPath.parent();
         executeCustomBuild(type, buildTargetPath, options, cb);
      }
      else
      {
         terminateWithError("Unrecognized build type: " + config.buildType);
      }
   }

   void executePackageBuild(const std::string& type,
                            const FilePath& packagePath,
                            const core::system::ProcessOptions& options,
                            const core::system::ProcessCallbacks& cb)
   {
      // validate that there is a DESCRIPTION file
      FilePath descFilePath = packagePath.childPath("DESCRIPTION");
      if (!descFilePath.exists())
      {
         boost::format fmt ("ERROR: The build directory does "
                            "not contain a DESCRIPTION\n"
                            "file so cannot be built as a package.\n\n"
                            "Build directory: %1%\n");
         terminateWithError(boost::str(
                 fmt % module_context::createAliasedPath(packagePath)));
         return;
      }

      // get package info
      Error error = pkgInfo_.read(packagePath);
      if (error)
      {
         terminateWithError("Reading package DESCRIPTION", error);
         return;
      }

      if (type == kRoxygenizePackage)
      {
         if (roxygenize(packagePath))
            enqueBuildCompleted();
      }
      else
      {
         if (roxygenizeRequired(type))
         {
            if (!roxygenize(packagePath))
               return;
         }

         // build the package
         buildPackage(type, packagePath, options, cb);
      }
   }

   bool roxygenizeRequired(const std::string& type)
   {
      if (!projectConfig().packageRoxygenize.empty())
      {
         if ((type == kBuildAndReload || type == kRebuildAll) &&
             options_.autoRoxygenizeForBuildAndReload)
         {
            return true;
         }
         else if ( (type == kBuildSourcePackage ||
                    type == kBuildBinaryPackage) &&
                   options_.autoRoxygenizeForBuildPackage)
         {
            return true;
         }
         else if ( (type == kCheckPackage) &&
                   options_.autoRoxygenizeForCheck)
         {
            return true;
         }
         else
         {
            return false;
         }
      }
      else
      {
         return false;
      }
   }

   bool roxygenize(const FilePath& packagePath)
   {
      // build the call to roxygenize
      std::vector<std::string> roclets;
      boost::algorithm::split(roclets,
                              projectConfig().packageRoxygenize,
                              boost::algorithm::is_any_of(","));
      BOOST_FOREACH(std::string& roclet, roclets)
      {
         roclet = "'" + roclet + "'";
      }
      boost::format fmt("roxygenize('.', roclets=c(%1%))");
      std::string roxygenizeCall = boost::str(
         fmt % boost::algorithm::join(roclets, ", "));

      // show the user the call to roxygenize
      enqueCommandString(roxygenizeCall);
      enqueBuildOutput("* checking for changes ... ");

      // format the command to send to R
      boost::format cmdFmt(
         "capture.output(suppressPackageStartupMessages("
              "{library(roxygen2); %1%;}"
          "))");
      std::string cmd = boost::str(cmdFmt % roxygenizeCall);


      // change to the package dir (and make sure we restore
      // before leaving the function)
      RestoreCurrentPathScope restorePathScope(
                                 module_context::safeCurrentPath());
      Error error = packagePath.makeCurrentPath();
      if (error)
      {
         terminateWithError("setting the working directory for roxygenize",
                            error);
         return false;
      }

      // execute it
      std::string output;
      error = r::exec::evaluateString(cmd, &output);
      if (error && (error.code() != r::errc::NoDataAvailableError))
      {
         enqueBuildOutput("ERROR\n\n");
         terminateWithError(r::endUserErrorMessage(error));
         return false;
      }
      else
      {
         // update progress
         enqueBuildOutput("DONE\n");

         // show output
         enqueBuildOutput(output + (output.empty() ? "\n" : "\n\n"));

         return true;
      }
   }

   void buildPackage(const std::string& type,
                     const FilePath& packagePath,
                     const core::system::ProcessOptions& options,
                     const core::system::ProcessCallbacks& cb)
   {      

      // if this action is going to INSTALL the package then on
      // windows we need to unload the library first
#ifdef _WIN32
      if (packagePath.childPath("src").exists() &&
         (type == kBuildAndReload || type == kRebuildAll ||
          type == kBuildBinaryPackage))
      {
         std::string pkg = pkgInfo_.name();
         Error error = r::exec::RFunction(".rs.forceUnloadPackage", pkg).call();
         if (error)
            LOG_ERROR(error);
      }
#endif

      // use both the R and gcc error parsers
      CompileErrorParsers parsers;
      parsers.add(rErrorParser(packagePath.complete("R")));
      parsers.add(gccErrorParser(packagePath.complete("src")));
      initErrorParser(packagePath, parsers);

      // make a copy of options so we can customize the environment
      core::system::ProcessOptions pkgOptions(options);
      core::system::Options childEnv;
      core::system::environment(&childEnv);

      // ensure consistent collation and sort orders accross all
      // package builds (devtools does this as well)
      core::system::setenv(&childEnv,"LC_ALL", "C");

      // allow child process to inherit our R_LIBS
      std::string libPaths = libPathsString();
      if (!libPaths.empty())
         core::system::setenv(&childEnv, "R_LIBS", libPaths);

      // prevent spurious cygwin warnings on windows
#ifdef _WIN32
      core::system::setenv(&childEnv, "CYGWIN", "nodosfilewarning");
#endif

      // add r tools to path if necessary
      addRtoolsToPathIfNecessary(&childEnv);

      pkgOptions.environment = childEnv;

      // get R bin directory
      FilePath rBinDir;
      Error error = module_context::rBinDir(&rBinDir);
      if (error)
      {
         terminateWithError("attempting to locate R binary", error);
         return;
      }

      // build command
      if (type == kBuildAndReload || type == kRebuildAll)
      {
         // restart R after build is completed
         restartR_ = true;

         // build command
         RCommand rCmd(rBinDir);
         rCmd << "INSTALL";

         // add --preclean if this is a rebuild all
         if (type == kRebuildAll)
            rCmd << "--preclean";

         // add extra args if provided
         std::string extraArgs = projectConfig().packageInstallArgs;
         rCmd << extraArgs;

         // add filename as a FilePath so it is escaped
         rCmd << FilePath(packagePath.filename());

         // show the user the command
         enqueCommandString(rCmd.commandString());

         // run R CMD INSTALL <package-dir>
         module_context::processSupervisor().runCommand(rCmd.shellCommand(),
                                                        pkgOptions,
                                                        cb);
      }

      else if (type == kBuildSourcePackage)
      {
         // compose the build command
         RCommand rCmd(rBinDir);
         rCmd << "build";

         // add extra args if provided
         std::string extraArgs = projectConfig().packageBuildArgs;
         rCmd << extraArgs;

         // add filename as a FilePath so it is escaped
         rCmd << FilePath(packagePath.filename());

         // show the user the command
         enqueCommandString(rCmd.commandString());

         // set a success message
         successMessage_ = buildPackageSuccessMsg("Source");

         // run R CMD build <package-dir>
         module_context::processSupervisor().runCommand(rCmd.shellCommand(),
                                                        pkgOptions,
                                                        cb);
      }

      else if (type == kBuildBinaryPackage)
      {
         // compose the INSTALL --binary
         RCommand rCmd(rBinDir);
         rCmd << "INSTALL";
         rCmd << "--build";
         rCmd << "--preclean";

         // add extra args if provided
         std::string extraArgs = projectConfig().packageBuildBinaryArgs;
         rCmd << extraArgs;

         // add filename as a FilePath so it is escaped
         rCmd << FilePath(packagePath.filename());

         // show the user the command
         enqueCommandString(rCmd.commandString());

         // set a success message
         successMessage_ = "\n" + buildPackageSuccessMsg("Binary");

         // run R CMD INSTALL --build <package-dir>
         module_context::processSupervisor().runCommand(rCmd.shellCommand(),
                                                        pkgOptions,
                                                        cb);
      }

      else if (type == kCheckPackage)
      {
         // first build then check

         // compose the build command
         RCommand rCmd(rBinDir);
         rCmd << "build";

         // add extra args if provided
         rCmd << projectConfig().packageBuildArgs;

         // add --no-manual and --no-vignettes if they are in the check options
         std::string checkArgs = projectConfig().packageCheckArgs;
         if (checkArgs.find("--no-manual") != std::string::npos)
            rCmd << "--no-manual";
         if (checkArgs.find("--no-vignettes") != std::string::npos)
            rCmd << "--no-vignettes";

         // add filename as a FilePath so it is escaped
         rCmd << FilePath(packagePath.filename());

         // compose the check command (will be executed by the onExit
         // handler of the build cmd)
         RCommand rCheckCmd(rBinDir);
         rCheckCmd << "check";

         // add extra args if provided
         std::string extraArgs = projectConfig().packageCheckArgs;
         rCheckCmd << extraArgs;

         // add filename as a FilePath so it is escaped
         rCheckCmd << FilePath(pkgInfo_.sourcePackageFilename());

         // special callback for build result
         core::system::ProcessCallbacks buildCb = cb;
         buildCb.onExit =  boost::bind(&Build::onBuildForCheckCompleted,
                                       Build::shared_from_this(),
                                       _1,
                                       rCheckCmd,
                                       pkgOptions,
                                       buildCb);

         // show the user the command
         enqueCommandString(rCmd.commandString());

         // set a success message
         successMessage_ = "R CMD check succeeded\n";

         // bind a success function if appropriate
         if (userSettings().cleanupAfterRCmdCheck())
         {
            successFunction_ = boost::bind(&Build::cleanupAfterCheck,
                                           Build::shared_from_this(),
                                           pkgInfo_);
         }

         if (userSettings().viewDirAfterRCmdCheck())
         {
            failureFunction_ = boost::bind(&Build::viewDirAfterFailedCheck,
                                           Build::shared_from_this(),
                                           pkgInfo_);
         }

         // run the source build
         module_context::processSupervisor().runCommand(rCmd.shellCommand(),
                                                        pkgOptions,
                                                        buildCb);
      }
   }


   void onBuildForCheckCompleted(
                         int exitStatus,
                         const RCommand& checkCmd,
                         const core::system::ProcessOptions& checkOptions,
                         const core::system::ProcessCallbacks& checkCb)
   {
      if (exitStatus == EXIT_SUCCESS)
      {
         // show the user the buld command
         enqueCommandString(checkCmd.commandString());

         // run the check
         module_context::processSupervisor().runCommand(checkCmd.shellCommand(),
                                                        checkOptions,
                                                        checkCb);
      }
      else
      {
         terminateWithErrorStatus(exitStatus);
      }
   }


   void cleanupAfterCheck(const r_util::RPackageInfo& pkgInfo)
   {
      // compute paths
      FilePath buildPath = projects::projectContext().buildTargetPath().parent();
      FilePath srcPkgPath = buildPath.childPath(pkgInfo.sourcePackageFilename());
      FilePath chkDirPath = buildPath.childPath(pkgInfo.name() + ".Rcheck");

      // cleanup
      Error error = srcPkgPath.removeIfExists();
      if (error)
         LOG_ERROR(error);
      error = chkDirPath.removeIfExists();
      if (error)
         LOG_ERROR(error);
   }

   void viewDirAfterFailedCheck(const r_util::RPackageInfo& pkgInfo)
   {
      FilePath buildPath = projects::projectContext().buildTargetPath().parent();
      FilePath chkDirPath = buildPath.childPath(pkgInfo.name() + ".Rcheck");

      json::Object dataJson;
      dataJson["directory"] = module_context::createAliasedPath(chkDirPath);
      dataJson["activate"] = true;
      ClientEvent event(client_events::kDirectoryNavigate, dataJson);

      module_context::enqueClientEvent(event);
   }

   void executeMakefileBuild(const std::string& type,
                             const FilePath& targetPath,
                             const core::system::ProcessOptions& options,
                             const core::system::ProcessCallbacks& cb)
   {
      // validate that there is a Makefile file
      FilePath makefilePath = targetPath.childPath("Makefile");
      if (!makefilePath.exists())
      {
         boost::format fmt ("ERROR: The build directory does "
                            "not contain a Makefile\n"
                            "so the target cannot be built.\n\n"
                            "Build directory: %1%\n");
         terminateWithError(boost::str(
                 fmt % module_context::createAliasedPath(targetPath)));
         return;
      }

      // install the gcc error parser
      initErrorParser(targetPath, gccErrorParser(targetPath));

      std::string make = "make";
      if (!options_.makefileArgs.empty())
         make += " " + options_.makefileArgs;

      std::string makeClean = make + " clean";

      std::string cmd;
      if (type == "build-all")
      {
         cmd = make;
      }
      else if (type == "clean-all")
      {
         cmd = makeClean;
      }
      else if (type == "rebuild-all")
      {
         cmd = shell_utils::join_and(makeClean, make);
      }

      module_context::processSupervisor().runCommand(cmd,
                                                     options,
                                                     cb);
   }

   void executeCustomBuild(const std::string& type,
                           const FilePath& customScriptPath,
                           const core::system::ProcessOptions& options,
                           const core::system::ProcessCallbacks& cb)
   {
      module_context::processSupervisor().runCommand(
                           shell_utils::ShellCommand(customScriptPath),
                           options,
                           cb);
   }


   void terminateWithErrorStatus(int exitStatus)
   {
      boost::format fmt("\nExited with status %1%.\n\n");
      enqueBuildOutput(boost::str(fmt % exitStatus));
      enqueBuildCompleted();
   }

   void terminateWithError(const std::string& context,
                           const Error& error)
   {
      std::string msg = "Error " + context + ": " + error.summary();
      terminateWithError(msg);
   }

   void terminateWithError(const std::string& msg)
   {
      enqueBuildOutput(msg);
      enqueBuildCompleted();
   }

public:
   virtual ~Build()
   {
   }

   bool isRunning() const { return isRunning_; }

   const std::string& output() const { return output_; }

   const std::string& errorsBaseDir() const { return errorsBaseDir_; }
   const json::Array& errorsAsJson() const { return errorsJson_; }

   void terminate()
   {
      enqueBuildOutput("\n");
      terminationRequested_ = true;
   }

private:
   bool onContinue()
   {
      return !terminationRequested_;
   }

   void onOutput(const std::string& output)
   {
      enqueBuildOutput(output);
   }

   void onCompleted(int exitStatus)
   {
      // call the error parser if one has been specified
      if (errorParser_)
      {
         std::vector<CompileError> errors = errorParser_(output_);
         if (!errors.empty())
         {
            errorsJson_ = compileErrorsAsJson(errors);
            enqueBuildErrors(errorsJson_);
         }
      }

      if (exitStatus != EXIT_SUCCESS)
      {
         boost::format fmt("\nExited with status %1%.\n\n");
         enqueBuildOutput(boost::str(fmt % exitStatus));

         // never restart R after a failed build
         restartR_ = false;

         // take other actions
         if (failureFunction_)
            failureFunction_();
      }
      else
      {
         if (!successMessage_.empty())
            enqueBuildOutput(successMessage_ + "\n");

         if (successFunction_)
            successFunction_();
      }

      enqueBuildCompleted();
   }

   void enqueBuildOutput(const std::string& output)
   {
      output_.append(output);

      ClientEvent event(client_events::kBuildOutput, output);
      module_context::enqueClientEvent(event);
   }

   void enqueCommandString(const std::string& cmd)
   {
      enqueBuildOutput("==> " + cmd + "\n\n");
   }

   void enqueBuildErrors(const json::Array& errors)
   {
      json::Object jsonData;
      jsonData["base_dir"] = errorsBaseDir_;
      jsonData["errors"] = errors;

      ClientEvent event(client_events::kBuildErrors, jsonData);
      module_context::enqueClientEvent(event);
   }

   void enqueBuildCompleted()
   {
      isRunning_ = false;

      // enque event
      std::string afterRestartCommand;
      if (restartR_)
         afterRestartCommand = "library(" + pkgInfo_.name() + ")";
      json::Object dataJson;
      dataJson["restart_r"] = restartR_;
      dataJson["after_restart_command"] = afterRestartCommand;
      ClientEvent event(client_events::kBuildCompleted, dataJson);
      module_context::enqueClientEvent(event);
   }

   const r_util::RProjectConfig& projectConfig()
   {
      return projects::projectContext().config();
   }

   std::string buildPackageSuccessMsg(const std::string& type)
   {
      FilePath writtenPath = projects::projectContext().buildTargetPath().parent();
      std::string written = module_context::createAliasedPath(writtenPath);
      if (written == "~")
         written = writtenPath.absolutePath();

      return type + " package written to " + written;
   }

   void initErrorParser(const FilePath& baseDir, CompileErrorParser parser)
   {
      // set base dir -- make sure it ends with a / so the slash is
      // excluded from error display
      errorsBaseDir_ = module_context::createAliasedPath(baseDir);
      if (!errorsBaseDir_.empty() &&
          !boost::algorithm::ends_with(errorsBaseDir_, "/"))
      {
         errorsBaseDir_.append("/");
      }

      errorParser_ = parser;
   }

private:
   bool isRunning_;
   bool terminationRequested_;
   std::string output_;
   CompileErrorParser errorParser_;
   std::string errorsBaseDir_;
   json::Array errorsJson_;
   r_util::RPackageInfo pkgInfo_;
   projects::RProjectBuildOptions options_;
   std::string successMessage_;
   boost::function<void()> successFunction_;
   boost::function<void()> failureFunction_;
   bool restartR_;
};

boost::shared_ptr<Build> s_pBuild;


bool isBuildRunning()
{
   return s_pBuild && s_pBuild->isRunning();
}

Error startBuild(const json::JsonRpcRequest& request,
                 json::JsonRpcResponse* pResponse)
{
   // get type
   std::string type;
   Error error = json::readParam(request.params, 0, &type);
   if (error)
      return error;

   // if we have a build already running then just return false
   if (isBuildRunning())
   {
      pResponse->setResult(false);
   }
   else
   {
      s_pBuild = Build::create(type);
      pResponse->setResult(true);
   }

   return Success();
}



Error terminateBuild(const json::JsonRpcRequest& request,
                     json::JsonRpcResponse* pResponse)
{
   if (isBuildRunning())
      s_pBuild->terminate();

   pResponse->setResult(true);

   return Success();
}

Error devtoolsLoadAllPath(const json::JsonRpcRequest& request,
                     json::JsonRpcResponse* pResponse)
{
   // compute the path to use for devtools::load_all
   std::string loadAllPath;

   // start with the build target path
   FilePath buildTargetPath = projects::projectContext().buildTargetPath();
   FilePath currentPath = module_context::safeCurrentPath();

   // if the build target path and the current working directory
   // are the same then return "."
   if (buildTargetPath == currentPath)
   {
      loadAllPath = ".";
   }
   else if (buildTargetPath.isWithin(currentPath))
   {
      loadAllPath = buildTargetPath.relativePath(currentPath);
   }
   else
   {
      loadAllPath = module_context::createAliasedPath(buildTargetPath);
   }

   pResponse->setResult(loadAllPath);

   return Success();
}

void onSuspend(core::Settings* pSettings)
{
   std::string lastBuildOutput = s_pBuild ? s_pBuild->output() : "";
   pSettings->set("build-last-output", lastBuildOutput);

   json::Array buildLastErrors = s_pBuild ? s_pBuild->errorsAsJson()
                                          : json::Array();
   std::ostringstream ostr;
   json::write(buildLastErrors, ostr);
   pSettings->set("build-last-errors", ostr.str());

   pSettings->set("build-last-errors-base-dir",
                  s_pBuild ? s_pBuild->errorsBaseDir() : "" );
}

struct SuspendContext
{
   bool empty() const { return errors.empty() && output.empty(); }
   std::string errorsBaseDir;
   json::Array errors;
   std::string output;
};

SuspendContext s_suspendContext;


void onResume(const core::Settings& settings)
{
   s_suspendContext.output = settings.get("build-last-output");

   s_suspendContext.errorsBaseDir = settings.get("build-last-errors-base-dir");
   std::string buildLastErrors = settings.get("build-last-errors");
   if (!buildLastErrors.empty())
   {
      json::Value errorsJson;
      if (json::parse(buildLastErrors, &errorsJson) &&
          json::isType<json::Array>(errorsJson))
      {
         s_suspendContext.errors = errorsJson.get_array();
      }
   }
}

} // anonymous namespace


json::Value buildStateAsJson()
{
   if (s_pBuild)
   {
      json::Object stateJson;
      stateJson["running"] = s_pBuild->isRunning();
      stateJson["output"] = s_pBuild->output();
      stateJson["errors_base_dir"] = s_pBuild->errorsBaseDir();
      stateJson["errors"] = s_pBuild->errorsAsJson();
      return stateJson;
   }
   else if (!s_suspendContext.empty())
   {
      json::Object stateJson;
      stateJson["running"] = false;
      stateJson["output"] = s_suspendContext.output;
      stateJson["errors_base_dir"] = s_suspendContext.errorsBaseDir;
      stateJson["errors"] = s_suspendContext.errors;
      return stateJson;
   }
   else
   {
      return json::Value();
   }
}


SEXP rs_canBuildCpp()
{
   r::sexp::Protect rProtect;
   return r::sexp::create(module_context::canBuildCpp(), &rProtect);
}

Error initialize()
{
   R_CallMethodDef methodDef ;
   methodDef.name = "rs_canBuildCpp" ;
   methodDef.fun = (DL_FUNC) rs_canBuildCpp ;
   methodDef.numArgs = 0;
   r::routines::addCallMethod(methodDef);

   // add suspend handler
   addSuspendHandler(module_context::SuspendHandler(onSuspend, onResume));

   // install rpc methods
   using boost::bind;
   using namespace module_context;
   ExecBlock initBlock ;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "start_build", startBuild))
      (bind(registerRpcMethod, "terminate_build", terminateBuild))
      (bind(registerRpcMethod, "devtools_load_all_path", devtoolsLoadAllPath));
   return initBlock.execute();
}


} // namespace build
} // namespace modules

namespace module_context {

bool canBuildCpp()
{
   // try to build a simple c file to test whether we have build tools available
   FilePath cppPath = module_context::tempFile("test", "c");
   Error error = core::writeStringToFile(cppPath, "void test() {}\n");
   if (error)
   {
      LOG_ERROR(error);
      return false;
   }

   // get R bin directory
   FilePath rBinDir;
   error = module_context::rBinDir(&rBinDir);
   if (error)
   {
      LOG_ERROR(error);
      return false;
   }

   // try to run build tools
   RCommand rCmd(rBinDir);
   rCmd << "SHLIB";
   rCmd << cppPath.filename();

   core::system::ProcessOptions options;
   options.workingDir = cppPath.parent();
   core::system::Options childEnv;
   core::system::environment(&childEnv);
   addRtoolsToPathIfNecessary(&childEnv);
   options.environment = childEnv;

   core::system::ProcessResult result;
   error = core::system::runCommand(rCmd.commandString(), options, &result);
   if (error)
   {
      LOG_ERROR(error);
      return false;
   }

   return result.exitStatus == EXIT_SUCCESS;
}

}

} // namesapce session

