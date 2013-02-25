import tempfile
import TestRunner

suffixMap = {
  '.adb'   : 'ada',
  '.ads'   : 'ada',
  '.c'     : 'c',
  '.i'     : 'c',
  '.C'     : 'c++',
  '.cc'    : 'c++',
  '.cpp'   : 'c++',
  '.ii'    : 'c++',
  '.f'     : 'fortran',
  '.f03'   : 'fortran',
  '.f08'   : 'fortran',
  '.f90'   : 'fortran',
  '.f95'   : 'fortran',
  '.F'     : 'fortran',
  '.F03'   : 'fortran',
  '.F08'   : 'fortran',
  '.F90'   : 'fortran',
  '.F95'   : 'fortran',
  '.go'    : 'go',
  '.jar'   : 'java',
  '.class' : 'java',
  '.m'     : 'objective-c',
  '.mm'    : 'objective-c++',
}

def getLanguageForSuffix(suffix):
  return suffixMap[suffix]

def getSuffixesForLanguage(language):
  suffixes = []
  for suffix in suffixMap:
     if suffixMap[suffix] == language:
       suffixes.append(suffix)
  return suffixes

def isLanguageSupported(language, compiler):
  if language == 'ada':
    suffix='.ads'
  elif language == 'c':
    suffix='.c'
  elif language == 'c++':
    suffix='.cpp'
  elif language == 'fortran':
    suffix='.f'
  elif language == 'go':
    suffix='.go'
  elif language == 'objective-c':
    suffix='.m'
  elif language == 'objective-c++':
    suffix='.mm'
  else:
    return False

  # For most languages it suffices to try compiling an empty file however for
  # Ada and Go an empty file is not a valid compilation unit.
  source = tempfile.NamedTemporaryFile(mode='w+t', suffix=suffix)

  if language == 'ada':
    # Use an obscure package name, as if the package is called XYZ and a file
    # called XYZ.adb exists then the test will fail.
    source.write('package U5TE4J886W is end;\n')
  elif language == 'go':
    source.write('package main\n')

  # If something was written then ensure it is visible to the compiler process.
  source.flush()

  # The language is supported if the file compiles without error.
  out,err,exitCode = TestRunner.executeCommand([compiler, '-S', source.name])
  return exitCode == 0

def getSupportedLanguages(compiler):
  allLanguages = set(suffixMap.values())
  return [lang for lang in allLanguages if isLanguageSupported(lang, compiler)]
