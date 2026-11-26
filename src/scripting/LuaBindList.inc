// Lua バインドの登録表。各レーンは自分のブロックの中にだけ足す（ブロックの外は書き換えない）。
//   include する側（LuaBindingRegistry.cpp、W05-15）が X マクロを定義する。
// ==== lane-A begin ====
// ==== lane-A end ====
// ==== lane-B begin ====
UNO_LUA_BIND(BindPost)   // post.* / ssao.*（bind/Post.cpp）
// ==== lane-B end ====
// ==== lane-C begin ====
// ==== lane-C end ====
// ==== lane-D begin ====
// ==== lane-D end ====
