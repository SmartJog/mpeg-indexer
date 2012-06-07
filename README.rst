============
mpeg-indexer
============

Mpeg-indexer is a MPEG stream parser. It creates an index that allows exact
mapping of a timecode to a frame and vice-versa.


License
=======

Mpeg-indexer is released under the `GNU LGPL 2.1 <http://www.gnu.org/licenses/lgpl-2.1.html>`_.


Build and installation
=======================

Bootstrapping
-------------

Mpeg-indexer does not have a bootstrap system as it never got passed the
experimental stage.

Building
--------

Mpeg-indexer provides a small Makefile which should get you through compilation
easily::

    $ make


Authors
=======

Mpeg-indexer was started at SmartJog by Thomas Zaffran in 2007.

* Thomas Zaffran <thomas.zaffran@smartjog.com>
* Baptiste Coudurier <baptiste.coudurier@smartjog.com>


Index format
============

::

    Index file specifications :
    Index file is wrttien in little endian
    Magic Number : 0x534A2D494E444558 (SJ-INDEX in hexadecimal) -> 64 bits
    Version      : 0x0000                                       -> 8 bits
    First presented frame PTS                                   -> 64 bits
    First decoded frame DTS                                     -> 64 bits
    First frame number                                          -> 8 bits
    First second number                                         -> 8 bits
    First minute number                                         -> 8 bits
    First hours number                                          -> 8 bits
    Index Data :
        PTS                                                 -> 64 bits
        DTS                                                 -> 64 bits
        PES offset                                          -> 64 bits
        Frame Type                                          -> 8 bits
        Frame number in timecode                            -> 8 bits
        Seconds number in timecode                          -> 8 bits
        Minutes number in timecode                          -> 8 bits
        Hours number in timecode                            -> 8 bits

