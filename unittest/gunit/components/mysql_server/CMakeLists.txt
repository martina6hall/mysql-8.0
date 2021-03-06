# Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

IF(DISABLE_SHARED)
  RETURN()
ENDIF()

INCLUDE_DIRECTORIES(
  ${GMOCK_INCLUDE_DIRS}
# ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/components/example
  ${CMAKE_SOURCE_DIR}/components/mysql_server
)

# GMock source use undefined symbols in #if checks
IF(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-undef")
ENDIF()

REMOVE_DEFINITIONS(-DMYSQL_SERVER)

# Add tests
SET(TESTS registry dynamic_loader)

FOREACH(test ${TESTS})
  ADD_EXECUTABLE(${test}-t
    ${test}-t.cc
    ${CMAKE_SOURCE_DIR}/sql/rwlock_scoped_lock.cc
    ${CMAKE_SOURCE_DIR}/sql/server_component/mysql_string_service.cc
    ${CMAKE_SOURCE_DIR}/sql-common/sql_string.cc
    builtins_stubs.cc
  )
  TARGET_LINK_LIBRARIES(${test}-t
    ${CMAKE_THREAD_LIBS_INIT} gunit_small component_mysql_server)
  ADD_TEST(${test} ${test}-t)
ENDFOREACH()

MYSQL_ADD_COMPONENT(self_required_test_component
  self_required_test_component.cc
  NO_INSTALL MODULE)
MYSQL_ADD_COMPONENT(cyclic_dependency_test_component_1
  cyclic_dependency_test_component_1.cc
  NO_INSTALL MODULE)
MYSQL_ADD_COMPONENT(cyclic_dependency_test_component_2
  cyclic_dependency_test_component_2.cc
  NO_INSTALL MODULE)

SET(EXAMPLE_COMPONENTS
  component_example_component1
  component_example_component2
  component_example_component3
  component_self_required_test_component
  component_cyclic_dependency_test_component_1
  component_cyclic_dependency_test_component_2
  )

FOREACH(EXAMPLE_COMPONENT ${EXAMPLE_COMPONENTS})
  ADD_DEPENDENCIES(dynamic_loader-t ${EXAMPLE_COMPONENT})

  # Copy all libraries to dynamic_loader-t's binary directory. This directory is
  # on VS different from target directory even for components defined in this
  # CMakeLists file.
  GET_TARGET_PROPERTY(example_location ${EXAMPLE_COMPONENT} LOCATION)
  ADD_CUSTOM_COMMAND(
    TARGET dynamic_loader-t
    COMMAND ${CMAKE_COMMAND} -E copy
      ${example_location}
      ${CMAKE_CURRENT_BINARY_DIR}
  )
ENDFOREACH()
