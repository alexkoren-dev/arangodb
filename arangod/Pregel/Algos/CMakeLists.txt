target_sources(arango_pregel PRIVATE
  AsyncSCC.cpp
  ConnectedComponents.cpp
  HITS.cpp
  HITSKleinberg.cpp
  LabelPropagation.cpp
  LineRank.cpp
  PageRank.cpp
  RecoveringPageRank.cpp
  SCC.cpp
  SLPA.cpp
  SSSP.cpp
  ShortestPath.cpp
  WCC.cpp)

if (USE_MAINTAINER_MODE)
  target_sources(arango_pregel PRIVATE
    ReadWrite.cpp)
endif()

add_subdirectory(DMID)
add_subdirectory(EffectiveCloseness)
add_subdirectory(AIR)
