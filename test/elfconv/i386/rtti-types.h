#pragma once

struct Left { virtual ~Left() {} int left = 101; };
struct Single : Left { int value = 505; };
struct Right { virtual ~Right() {} int right = 202; };
struct Mixed : Left, Right { int value = 303; };
struct Unrelated { virtual ~Unrelated() {} };
struct VirtualBase { virtual ~VirtualBase() {} int value = 404; };
struct BranchA : virtual VirtualBase {};
struct BranchB : virtual VirtualBase {};
struct Diamond : BranchA, BranchB {};
