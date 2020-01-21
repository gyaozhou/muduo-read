#ifndef MUDUO_BASE_NONCOPYABLE_H
#define MUDUO_BASE_NONCOPYABLE_H

namespace muduo
{

// zhou: compare to "class copyable", any derived class of copyable should be a
//       object type.
class noncopyable
{
 public:
  // zhou: the parameter of copy construct must be reference.
  //       Otherwise, the argument assignment will be another copy construct.
  //       delete, we don't want compiler make a default implementation.
  noncopyable(const noncopyable&) = delete;
  void operator=(const noncopyable&) = delete;

  // zhou: the reason put them in "protected" is, prevent client to create object
  //       via it. We DO NOT want create object of "class nocopable".
  //       But the derived class could create object since their
  //       construct could be public, and invoke base class protected construct.
 protected:
  noncopyable() = default;
  ~noncopyable() = default;
};

}  // namespace muduo

#endif  // MUDUO_BASE_NONCOPYABLE_H
