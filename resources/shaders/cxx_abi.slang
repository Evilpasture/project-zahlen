// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module cxx_abi;

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxArrayAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxCArrayAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxQuatAttribute {
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxEnumAttribute {
    string name;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct CxxDefaultAttribute {
    string init;
};

[__AttributeUsage(_AttributeTargets.Struct)]
public struct CxxSkipAttribute {
};
