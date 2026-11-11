# CeeLing documentation

| Where | What it is | Read it when |
|---|---|---|
| [language-guide/](language-guide/README.md) | the guide: 18 chapters, every example checked by the test suite | you want to learn the language, or to know exactly what is and is not guaranteed |
| [design/](design/) | design records: why a part of the compiler is the way it is | you want the reasoning behind a decision, not the current behaviour |
| [internals/](internals/) | reference for people working on the compiler | you are writing a fact pack or changing the facts table |

Design records say at the top whether they are current. The guide and the code are the description of behaviour.

* [design/third-party-inference.md](design/third-party-inference.md): the reasoning behind library facts and what is not built (how to use them is in [guide chapter 11](language-guide/11-talking-to-cpp.md#who-vouches-for-a-fact)).
* [design/eigen-views.md](design/eigen-views.md): the first case of that, Eigen's view methods, and why it needed its own mechanism.
* [internals/facts-format.md](internals/facts-format.md): the `.clfacts` file format.

To contribute, see [CONTRIBUTING.md](../CONTRIBUTING.md).
