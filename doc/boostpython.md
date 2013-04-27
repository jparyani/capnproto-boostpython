---
layout: page
---

# Boost Python Runtime

The Cap'n Proto Boost Python C++ runtime provides a python interface to the
C++ runtime of Cap'n Proto. 

## Example Usage

For the Cap'n Proto definition:

```
struct Person {
  id @0 :UInt32;
  name @1 :Text;
  email @2 :Text;
  phones @3 :List(PhoneNumber);

  struct PhoneNumber {
    number @0 :Text;
    type @1 :Type;

    enum Type {
      mobile @0;
      home @1;
      work @2;
    }
  }

  employment @4 union {
    unemployed @5 :Void;
    employer @6 :Text;
    school @7 :Text;
    selfEmployed @8 :Void;
    # We assume that a person is only one of these.
  }
}

struct AddressBook {
  people @0 :List(Person);
}
```

You might write code like:

```python
import addressbook

def writeAddressBook(fd):
    message = addressbook.MallocMessageBuilder()
    addressBook = message.initRootAddressBook()
    people = addressBook.initPeople(2)

    alice = people[0]
    alice.id = 123
    alice.name = 'Alice'
    alice.email = 'alice@example.com'
    alicePhones = alice.initPhones(1)
    alicePhones[0].number = "555-1212"
    alicePhones[0].type = addressbook.Person.PhoneNumber.Type.MOBILE
    alice.employment.school = "MIT"

    bob = people[1]
    bob.id = 456
    bob.name = 'Bob'
    bob.email = 'bob@example.com'
    bobPhones = bob.initPhones(2)
    bobPhones[0].number = "555-4567"
    bobPhones[0].type = addressbook.Person.PhoneNumber.Type.HOME
    bobPhones[1].number = "555-7654" 
    bobPhones[1].type = addressbook.Person.PhoneNumber.Type.WORK
    bob.employment.unemployed = addressbook.Void.VOID # This is definitely bad, syntax will change at some point
    
    addressbook.writePackedMessageToFd(fd, message)

f = open('example', 'w')
writeAddressBook(f.fileno())

def printAddressBook(fd):
    message = addressbook.PackedFdMessageReader(f.fileno())
    addressBook = message.getRootAddressBook()
    
    for person in addressBook.people:
        print person.name, ':', person.email
        for phone in person.phones:
            print phone.type, ':', phone.number
            
        which = person.employment.which()
        print which
        
        if which == addressbook.Person.Employment.Which.UNEMPLOYED:
            print 'unemployed'
        elif which == addressbook.Person.Employment.Which.EMPLOYER:
            print 'employer:', person.employment.employer
        elif which == addressbook.Person.Employment.Which.SCHOOL:
            print 'student at:', person.employment.school
        elif which == addressbook.Person.Employment.Which.SELF_EMPLOYED:
            print 'unemployed'
        print
        
f = open('example', 'r')
printAddressBook(f.fileno())

```

## Installation

You need to install libboost_python, and make sure your libcapnprotocpp is 
compiled with -fpic
* read the instructions at [http://kentonv.github.io/capnproto/install.html](http://kentonv.github.io/capnproto/install.html) if you get confused 
* `sudo apt-get install libboost-python-dev` (or the equivalent for your distro. Tested working on ubuntu 12.10)
* clone this capnproto repo
* (Re)install the compiler:
  cd capnproto/compiler
  cabal install capnproto-compiler.cabal
* Configure the Capn Proto C++ runtime with -fpic and then (re)install it:
  cd ../c++
  CXXFLAGS="-O2 -DNDEBUG -I$HOME/gtest-install/include -fpic" LDFLAGS="-L$HOME/gtest-install/lib" ./configure; make clean; sudo make install`

## Generating Code

To generate C++ code from your `.capnp` [interface definition](language.html), run:

    capnpc -oc++ myproto.capnp && capnpc -oboost-python myproto.capnp

This will create `myproto.capnp.boost-python.c++`, `myproto.capnp.h`, and `myproto.capnp.c++` in the same directory as `myproto.capnp`. 
You have to run both the normal C++ and the boost python compiler separately. 

## Compiling Code

    g++ -shared  -fPIC --std=gnu++11 -lcapnproto   -lpython2.7 -lboost_python -I /usr/include/python2.7 myproto.capnp.boost-python.c++ myproto.capnp.c++ -o myproto.so 

Note the `-o myproto.so` part absolutely must match the name of your protocol ie. in this case `myproto`

## Primitive Types

Primitive types map to the obvious Python types:

* `Bool` -> `bool`
* `IntNN` -> `int` or `long`
* `UIntNN` -> `int` or `long`
* `FloatNN` -> `float`
* `Void` -> `your_module.Void.VOID` (An enum with one value)

