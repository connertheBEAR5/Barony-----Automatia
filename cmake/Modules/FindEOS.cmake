# - Try to find the EOS SDK
#
# Once done, this will define:
#
#  EOS_INCLUDE_DIR - the EOS include directory
#  EOS_LIBRARIES - The libraries needed to use EOS

set(EOS_SDK_ROOTS
	"${EOS_ROOT}"
	"${EOS_DIR}"
	"$ENV{EOSROOT}"
	"$ENV{EOS_ROOT}"
	"$ENV{EOS_DIR}"
)

if (NOT EOS_INCLUDE_DIR OR NOT EOS_LIBRARIES)
	set(LIB_SEARCH_PATHS
		~/Library/Frameworks
		/Library/Frameworks
		/usr/lib
		/usr/lib64
		/usr/local/lib
		/usr/local/lib64
		$ENV{EOSROOT}/SDK
		$ENV{EOS_ROOT}/SDK
		$ENV{EOS_DIR}/SDK
		${EOS_ROOT}/SDK
		${EOS_DIR}/SDK
	)
	FIND_PATH(EOS_INCLUDE_DIR eos_sdk.h
		HINTS ${EOS_SDK_ROOTS}
		PATH_SUFFIXES SDK/Include
		PATHS
		/usr/include
		/usr/local/include
		DOC "Include path for EOS"
	)

	if (WIN32)
		if (CMAKE_SIZEOF_VOID_P EQUAL 4)
			FIND_LIBRARY(EOS_LIBRARY NAMES EOSSDK-Win32-Shipping
				HINTS ${EOS_SDK_ROOTS}
				PATH_SUFFIXES SDK/Lib
				DOC "EOS library name"
			)
			MESSAGE(STATUS "EOS: selecting 32-bit Windows library")
		else ()
			FIND_LIBRARY(EOS_LIBRARY NAMES EOSSDK-Win64-Shipping
				HINTS ${EOS_SDK_ROOTS}
				PATH_SUFFIXES SDK/Lib
				DOC "EOS library name"
			)
			MESSAGE(STATUS "EOS: selecting 64-bit Windows library")
		endif()
	elseif (APPLE)
		FIND_LIBRARY(EOS_LIBRARY NAMES libEOSSDK-Mac-Shipping.dylib
			HINTS ${EOS_SDK_ROOTS}
			PATH_SUFFIXES SDK/Bin
			DOC "EOS library name"
		)
	else () # TODO: Technically, Linux portion. I don't know what the EOS Linux SDK looks like yet, since the launcher doesn't even run on Linux, but we'll probably get to this eventually?
		FIND_LIBRARY(EOS_LIBRARY NAMES libEOSSDK-Linux-Shipping.so
			HINTS ${EOS_SDK_ROOTS}
			PATH_SUFFIXES SDK/Bin
			DOC "EOS library name"
		)
	endif ()
	if (EOS_LIBRARY)
		set(EOS_LIBRARIES ${EOS_LIBRARY})
	endif ()
	MARK_AS_ADVANCED(EOS_INCLUDE_DIR EOS_LIBRARIES)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EOS DEFAULT_MSG EOS_INCLUDE_DIR EOS_LIBRARIES)
