#!/usr/bin/env python3
#
#===- test_rv.py ---------------------------------------------------===//
#
#                     The Region Vectorizer
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
# @authors simon
#



from glob import glob
from binaries import *
from os import path

if len(sys.argv) > 1:
  patterns = sys.argv[1:]
else:
  patterns = ["suite/*.c*"]

def wholeFunctionVectorize(srcFile, argMappings):
  baseName = path.basename(srcFile)
  destFile = "build/" + baseName + ".wfv.ll"
  logPrefix =  "logs/"  + baseName + ".wfv"
  scalarName = "foo"
  runWFV(srcFile, destFile, scalarName, argMappings, logPrefix)
  return destFile

def outerLoopVectorize(srcFile, loopDesc):
  baseName = path.basename(srcFile)
  destFile = "build/" + baseName + ".loopvec.ll"
  logPrefix =  "logs/"  + baseName + ".loopvec"
  scalarName = "foo"
  runOuterLoopVec(srcFile, destFile, scalarName, loopDesc, logPrefix)
  return destFile;

def executeWFVTest(scalarLL, options):
  sigInfo = options.split("_")
  launchCode = sigInfo[0]
  shapes = sigInfo[1]
  testBC = wholeFunctionVectorize(scalarLL, shapes)
  return runWFVTest(testBC, launchCode)

def executeOuterLoopTest(scalarLL, options):
  sigInfo = options.split("_")
  launchCode = sigInfo[0]
  loopHint = sigInfo[1]

  vectorIR = outerLoopVectorize(scalarLL, loopHint)

  scalarRes = runOuterLoopTest(scalarLL, launchCode, "scalar")
  vectorRes = runOuterLoopTest(vectorIR, launchCode, "loopvec")

  if scalarRes is None or vectorRes is None:
    return False

  return scalarRes == vectorRes


print("-- RV tester --")
for pattern in patterns:
  for testCase in glob(pattern):
    baseName = path.basename(testCase)
    print("- {}".format(baseName))
    parts = baseName.split("-")
    options = parts[-1].split(".")[0]
    mode = parts[-2]
    rest = "-".join(parts[0:-2])

    scalarLL = buildScalarIR(testCase)

    if mode == "wfv":
      success = executeWFVTest(scalarLL, options)
    elif mode == "loop":
      success = executeOuterLoopTest(scalarLL, options)
    if success:
        print("\tpassed!")
    else:
        print("\tfailed!")



