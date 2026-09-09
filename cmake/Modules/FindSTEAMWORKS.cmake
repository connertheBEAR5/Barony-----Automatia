# - Try to find the steamworks library
#
# Once done, this will define:
#
#  STEAMWORKS_INCLUDE_DIR - the Steamworks include directory
#  STEAMWORKS_LIBRARIES - The libraries needed to use Steamworks

set(STEAMWORKS_SDK_ROOTS
	"${STEAMWORKS_ROOT}"
	"${STEAMWORKS_DIR}"
	"$ENV{STEAMWORKSROOT}"
	"$ENV{STEAMWORKS_ROOT}"
	"$ENV{STEAMWORKS_DIR}"
)

if (NOT STEAMWORKS_INCLUDE_DIR OR NOT STEAMWORKS_LIBRARIES)
	set(LIB_SEARCH_PATHS
		~/Library/Frameworks
		/Library/Frameworks
		/usr/lib
		/usr/lib64
		/usr/local/lib
		/usr/local/lib64
		$ENV{STEAMWORKSROOT}/sdk/redistributable_bin/linux64 #I don't like this. TODO: Make it determine 64/32 bit automatically.
		$ENV{STEAMWORKS_ROOT}/sdk/redistributable_bin/linux64
		$ENV{STEAMWORKS_DIR}/sdk/redistributable_bin/linux64
		${STEAMWORKS_ROOT}/sdk/redistributable_bin/linux64
		${STEAMWORKS_DIR}/sdk/redistributable_bin/linux64
		#$ENV{STEAMWORKSROOT}/sdk/redistributable_bin/linux32
		#$ENV{STEAMWORKS_ROOT}/sdk/redistributable_bin/linux32
		#$ENV{STEAMWORKS_DIR}/sdk/redistributable_bin/linux32
	)
	FIND_PATH(STEAMWORKS_INCLUDE_DIR steam/steam_api.h
		HINTS ${STEAMWORKS_SDK_ROOTS}
		PATH_SUFFIXES sdk/public
		PATHS
		/usr/include
		/usr/local/include
		DOC "Include path for Steamworks"
	)

	if (WIN32)
		if (CMAKE_SIZEOF_VOID_P EQUAL 4)
			FIND_LIBRARY(STEAMWORKS_LIBRARY NAMES steam_api
				HINTS ${STEAMWORKS_SDK_ROOTS}
				PATH_SUFFIXES sdk/redistributable_bin
				DOC "Steamworks library name"
			)
			MESSAGE(STATUS "Steamworks: selecting 32-bit Windows library")
		else ()
			FIND_LIBRARY(STEAMWORKS_LIBRARY NAMES steam_api64
				HINTS ${STEAMWORKS_SDK_ROOTS}
				PATH_SUFFIXES sdk/redistributable_bin/win64
				DOC "Steamworks library name"
			)
			MESSAGE(STATUS "Steamworks: selecting 64-bit Windows library")
		endif()
	elseif (APPLE)
		FIND_LIBRARY(STEAMWORKS_LIBRARY NAMES steam_api
			HINTS ${STEAMWORKS_SDK_ROOTS}
			PATH_SUFFIXES sdk/redistributable_bin sdk/redistributable_bin/osx32
			DOC "Steamworks library name"
		)
	else ()
		FIND_LIBRARY(STEAMWORKS_LIBRARY NAMES steam_api
			PATHS ${LIB_SEARCH_PATHS}
			DOC "Steamworks library name"
		)
	endif ()
	if (STEAMWORKS_LIBRARY)
		set(STEAMWORKS_LIBRARIES ${STEAMWORKS_LIBRARY})
	endif ()
	MARK_AS_ADVANCED(STEAMWORKS_INCLUDE_DIR STEAMWORKS_LIBRARIES)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(STEAMWORKS DEFAULT_MSG STEAMWORKS_INCLUDE_DIR STEAMWORKS_LIBRARIES)
