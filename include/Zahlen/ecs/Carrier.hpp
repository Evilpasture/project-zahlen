// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

// The light half of the system-graph header split: everything a translation
// unit needs to NAME the graph, and nothing that runs it.
//
// SystemGraph.hpp carries the reflection machinery (CallableInspector and
// friends), whose splice-based type aliases GCC 16 cannot reconcile across a
// module boundary: any module interface whose global fragment reaches them --
// Engine.hpp includes SystemContext.hpp, which used to include SystemGraph.hpp
// -- fails its implementation units with "conflicting imported declaration"
// as soon as the purview exports anything whose type closure touches the
// graph (e.g. Register(Engine&)). Clang merges the two copies; GCC 16 does
// not. So the carrier, the empty bundle, and the graph forward declaration
// live here, with no reflection includes, and SystemContext.hpp includes only
// this header. Code that runs systems includes SystemGraph.hpp directly and
// is unaffected: with no BMI copy to merge against, the textual definitions
// stand alone. Do not include reflection or ECS machinery headers from here.

namespace ZHLN {
struct Frame;
}

namespace ZHLN::ECS {

class Registry;

template <typename Services>
class SystemGraph;

// A graph that provides no services: the graph a unit test builds when it wants
// to exercise ordering, queries and frame parameters and nothing else. Systems
// that ask for a service cannot be added to it -- that is a compile error, not a
// null pointer at run time.
struct NoServices {};

namespace TemplatedDetail {

// The family id of a Local<T> slot. Locals are not components and are never
// stored in the registry; the id exists so a node's state can be found by type
// with the same by-family lookup the scheduler uses for services.
template <typename T>
struct LocalToken {};

struct LocalSlotDesc;

// What a system is handed to resolve its own state: the block the graph
// allocated for this node, and the layout its signature asked for. The pointer
// belongs to one node and one invocation; nothing else may read it.
struct LocalStateView {
    void*                block = nullptr;
    const LocalSlotDesc* slots = nullptr;
    size_t               count = 0;
};

} // namespace TemplatedDetail

// What the executor hands a system. Two references and a frame: the registry the
// graph runs against, the services the graph was built with, and this
// execution's clock and scratch. No engine pointers, no null service slots.
//
// `local` is this node's own state (see Local<T>): the graph fills it in for each
// invocation, and it is the only way a system reaches state that belongs to it
// rather than to the world.
//
// The `typedef` names the services type so a resolver can ask, of the carrier it
// was handed, whether those services provide what it was asked to resolve.
template <typename ServicesT>
struct Carrier {
    using Services = ServicesT;

    ECS::Registry&                  registry;
    ServicesT&                      services;
    const ZHLN::Frame&              frame;
    TemplatedDetail::LocalStateView local {};
};

} // namespace ZHLN::ECS
