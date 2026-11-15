# IfcOpenShell batches openings by size. A single troublesome opening can make
# the whole batch fail, so retry its members individually before giving up.
# Keep the upstream submodule pristine and fail configuration if it changes.
set(_kernel "${IFCOPENSHELL_SOURCE}/src/ifcgeom/kernels/opencascade/opencascade_kernel.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kernel}")
file(READ "${_kernel}" _source)
string(REPLACE "\r\n" "\n" _source "${_source}")
string(REPLACE "#include <BOPAlgo_MakerVolume.hxx>" "#include <BOPAlgo_MakerVolume.hxx>\n#include <Bnd_Box.hxx>\n#include <BRepBndLib.hxx>\n#include <BRepAlgoAPI_Common.hxx>\n#include <BRepCheck_Analyzer.hxx>\n#include <BRepGProp.hxx>\n#include <GProp_GProps.hxx>\n#include <TopExp_Explorer.hxx>" _source "${_source}")
set(_batch_old "if (it == opening_vector.end() || jt->first / it->first > 10.) {")
set(_batch_new "if (it == opening_vector.end() || jt->first / it->first > 10. || std::distance(jt, it) >= 2) {")
string(FIND "${_source}" "${_batch_old}" _batch_at)
if(_batch_at EQUAL -1)
    message(FATAL_ERROR "IfcOpenShell opening batch no longer matches the checked-out source")
endif()
string(REPLACE "${_batch_old}" "${_batch_new}" _source "${_source}")
set(_old [=[logger_.message(ifcopenshell::logger::LOG_ERROR, "GEO", 192, "Opening subtraction failed for " + boost::lexical_cast<std::string>(std::distance(jt, it)) + " openings", entity);]=])
set(_new [=[std::vector<TopoDS_Shape> retry_openings;
							std::vector<TopoDS_Shape> candidates;
							Bnd_Box result_box;
							BRepBndLib::Add(result, result_box);
							for (auto one = jt; one != it; ++one) {
								Bnd_Box opening_box;
								BRepBndLib::Add(one->second, opening_box);
								if (!result_box.IsVoid() && !opening_box.IsVoid() && result_box.IsOut(opening_box)) continue;
								candidates.push_back(one->second);
							}
							for (auto one = candidates.rbegin(); one != candidates.rend(); ++one) {
								TopoDS_Shape single_result;
								if (util::boolean_operation(bst, result, *one, BOPAlgo_CUT, single_result)) {
									result = single_result;
								} else {
									retry_openings.push_back(*one);
								}
							}
							size_t failed_openings = 0;
							auto alternate = bst;
							alternate.attempt_2d = !bst.attempt_2d;
							for (const auto& opening : retry_openings) {
								TopoDS_Shape single_result;
								if (util::boolean_operation(alternate, result, opening, BOPAlgo_CUT, single_result)) {
									result = single_result;
								} else {
									// A failed cut is harmless only when a valid solid intersection has no volume.
									bool disjoint = false;
									if (TopExp_Explorer(result, TopAbs_SOLID).More()) {
										BRepAlgoAPI_Common common(result, opening);
										common.Build();
										if (common.IsDone() && !common.HasErrors() && !common.Shape().IsNull() && BRepCheck_Analyzer(common.Shape()).IsValid()) {
											GProp_GProps properties;
											BRepGProp::VolumeProperties(common.Shape(), properties);
											disjoint = std::abs(properties.Mass()) <= bst.precision * bst.precision * bst.precision;
										}
									}
									if (!disjoint) ++failed_openings;
								}
							}
							if (failed_openings) {
								logger_.message(ifcopenshell::logger::LOG_ERROR, "GEO", 192, "Opening subtraction failed for " + boost::lexical_cast<std::string>(failed_openings) + " openings after individual retries", entity);
							}]=])
string(FIND "${_source}" "${_old}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "IfcOpenShell opening retry no longer matches the checked-out source")
endif()
string(REPLACE "${_old}" "${_new}" _source "${_source}")
set(_patched "${CMAKE_CURRENT_BINARY_DIR}/ifcopenshell-opening-retry/opencascade_kernel.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ifcopenshell-opening-retry")
if(EXISTS "${_patched}")
    file(READ "${_patched}" _previous)
endif()
if(NOT "${_source}" STREQUAL "${_previous}")
    file(WRITE "${_patched}" "${_source}")
endif()
get_target_property(_sources geometry_kernel_opencascade SOURCES)
list(FIND _sources "${_kernel}" _source_index)
if(_source_index EQUAL -1)
    message(FATAL_ERROR "IfcOpenShell geometry kernel source was not found in its target")
endif()
list(REMOVE_ITEM _sources "${_kernel}")
set_property(TARGET geometry_kernel_opencascade PROPERTY SOURCES "${_sources}")
target_sources(geometry_kernel_opencascade PRIVATE "${_patched}")
target_include_directories(geometry_kernel_opencascade PRIVATE "${IFCOPENSHELL_SOURCE}/src/ifcgeom/kernels/opencascade")
