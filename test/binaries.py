#===- binaries.py ---------------------------------------------------===//
#
#                     The Region Vectorizer
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
# @authors simon
#

import os
from os import path
import shlex, subprocess, sys, errno

# set-up workspace
Debug=False

libRV="../lib/libRV.so"

def assureDir(path):
  try:
    os.makedirs(path)
  except OSError as exception:
    if exception.errno != errno.EEXIST:
      raise

assureDir("logs")
assureDir("build")

def shellCmd(cmdText, envModifier=None, logPrefix=None):
    if Debug:
      print("CMD {}".format(cmdText))
    processEnv=os.environ
    if envModifier:
        processEnv=dict(os.environ, **envModifier)
    cmd = shlex.split(cmdText)
    if logPrefix is None:
      # stdout
      proc = subprocess.Popen(cmd, env=processEnv)
    else:
      # write to log streams
      with open(logPrefix + ".out", "w") as fOut:
        with open(logPrefix + ".err", "w") as fErr:   
          proc = subprocess.Popen(cmd, stdout=fOut, stderr=fErr, env=processEnv)
    retCode=proc.wait()
    if retCode != 0:
        print("")
        print(cmdText)
    return retCode

def runForOutput(cmdText):
    if Debug:
      print("CMD {}".format(cmdText))
    cmd = shlex.split(cmdText)
    try:
        return True, subprocess.check_output(cmd)
    except CalledProcessError as err:
        return False, err.output

clangLine="clang++ -std=c++14 -march=native -m64 -O2 -fno-vectorize -fno-slp-vectorize"
cClangLine="clang -march=native -m64 -O2 -fno-vectorize -fno-slp-vectorize"

rvToolLine="./../bin/rvTool"

def rvClang(clangArgs):
   return shellCmd(clangLine + " -Xclang -load -Xclang " + libRV + " -O3 " + clangArgs)


def plainName(fileName):
    return os.path.basename(fileName).split(".")[0]

def buildScalarIR(srcFile):
    baseName = plainName(srcFile)
    scalarLL = "build/" + baseName + ".ll"
    compileToIR(srcFile, scalarLL)
    return scalarLL

def runOuterLoopVec(scalarLL, destFile, scalarName = "foo", loopDesc=None, logPrefix=None):
    baseName = plainName(scalarLL)
    cmd = rvToolLine + " -loopvec -i " + scalarLL
    if destFile:
      cmd = cmd + " -o " + destFile
    if scalarName:
      cmd = cmd + " -k " + scalarName
    if loopDesc:
      cmd = cmd + " -l " + loopDesc

    return shellCmd(cmd,  None, logPrefix)

def runWFV(scalarLL, destFile, scalarName = "foo", shapes=None, logPrefix=None):
    cmd = rvToolLine + " -wfv -i " + scalarLL
    if destFile:
      cmd = cmd + " -o " + destFile
    if scalarName:
      cmd = cmd + " -k " + scalarName
    if shapes:
      cmd = cmd + " -s " + shapes

    return shellCmd(cmd,  None, logPrefix)

launcherCache = set()

def requestLauncher(launchCode, prefix):
    launcherCpp = "launcher/" + prefix + "_" + launchCode + ".cpp"
    launcherLL= "build/verify_" + launchCode + ".ll"
    if launcherLL in launcherCache:
      return launcherLL
    else:
      retCode = compileToIR(launcherCpp, launcherLL)
      assert retCode == 0
      launcherCache.add(launcherLL)
    return launcherLL

def runWFVTest(testBC, launchCode):
    caseName = plainName(testBC)
    launcherLL = requestLauncher(launchCode, "verify")
    launcherBin = "./build/verify_" + caseName + ".bin"
    shellCmd(clangLine + " " + testBC + " " + launcherLL + " -o " + launcherBin)
    retCode = shellCmd(launcherBin)
    return retCode == 0

def runOuterLoopTest(testBC, launchCode, suffix):
    caseName = plainName(testBC)
    launcherLL = requestLauncher(launchCode, "loopverify")
    launcherBin = "./build/verify_" + caseName + "." + suffix + ".bin"
    success, hashText = runForOutput(clangLine + " " + testBC + " " + launcherLL + " -o " + launcherBin)
    return hashText if success else None

def compileToIR(srcFile, destFile):
    if srcFile[-2:] == ".c":
      return shellCmd(cClangLine + " " + srcFile + " -fno-unroll-loops -S -emit-llvm -c -o " + destFile)
    else:
      return shellCmd(clangLine + " " + srcFile + " -fno-unroll-loops -S -emit-llvm -c -o " + destFile)

def disassemble(bcFile,suffix):
    return shellCmd("llvm-dis " + bcFile, "logs/dis_" + suffix) == 0
    
def build_launcher(launcherBin, launcherLL, fooFile, suffix):
    return shellCmd(clangLine + " -fno-slp-vectorize -o " + launcherBin + " " + launcherLL + " " + fooFile, "logs/clang-launcher_" + suffix) == 0

def assemble(fooFile, destAsm, suffix):
    return shellCmd(clangLine + " -fno-slp-vectorize -c -S -o " + destAsm + " " + fooFile, "logs/clang-asm_" + suffix) == 0
