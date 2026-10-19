<!--
SPDX-License-Identifier: GPL-3.0-only
SPDX-FileCopyrightText: Copyright Logan oos Even
SPDX-FileCopyrightText: Copyright Hamish Coleman
SPDX-FileCopyrightText: Copyright Honey Bunny QT
-->

# Edge Authentication

Who may join a community?  By default, whoever knows its name (and, to
take part in its traffic, its key).  The supernode can check more than
that, in two ways:

- **always**: it protects each edge's MAC and IP address from being taken
  over by another edge while the first one is online;
- **with users and passwords**: each edge has a user name and password of
  its own, and the supernode lets in only the users it knows.

## Protection against spoofing

Every edge makes up a random ID when it starts, and sends it with each
registration.  The supernode remembers the ID that first registered a MAC
address, and takes a registration (or an unregistration) for that MAC
address only with the same ID.  Another edge that claims a MAC or IP
address already in use gets a refusal, and logs:

```
ERROR: authentication error, MAC or IP address already in use or not released yet by supernode
```

Federated supernodes pass the registrations on to each other, so this
holds across the federation.  Other edges never see the ID.

Someone who can watch the traffic between an edge and its supernode can
read the ID, though: for real protection, use users and passwords.

An edge that ends without unregistering - killed with `kill -9`, or its
host crashed - blocks its MAC and IP address until the supernode forgets
it, after a minute or two. An edge started again with the same addresses
gets the error above until then. End edges with Ctrl-C, `kill` (SIGTERM),
or `n3nctl -k PASSWORD stop`, and they unregister.
`supernode.spoofing_protection = false` turns the check off, for the
communities without users.

## Users and passwords

Each user has a password, from which, together with the user name,
`n3n-edge tools keygen` makes a public key (Curve25519).  The supernode
is given the public keys, the edges their user names and passwords - the
passwords never leave the edges.

An edge's MAC and IP address stay with the user that registered them:
another user - evilrabbit, with a password of its own but honeybunny's
MAC or address - gets the refusal above.  (Up to n3n-be 3.4.8, any user
of the community could take them over.)

### 1. Make the keys

For each user, with their password:

```
$ n3n-edge tools keygen honeybunny cutie
* honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa
$ n3n-edge tools keygen evilrabbit carrotthief
* evilrabbit JMhQ8icD-cRekgmAl0ylVJFOMaDIPXuEjIm3ajasHIe
```

(Please pick better passwords than honeybunny's.)

and for the federation, with its name (see
[Federation](Federation.md); give the supernodes one of their own, and
keep it secret):

```
$ n3n-edge tools keygen myFederation
auth.pubkey=XhnQB26arLouAf3VXjGoAPTjTRIgEJ01Yi30Fhuqc7K
```

### 2. The supernode

The users go into their community's section of the supernode's
configuration:

```ini
[supernode]
federation = myFederation

[community netleo]
user = honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa
user = evilrabbit JMhQ8icD-cRekgmAl0ylVJFOMaDIPXuEjIm3ajasHIe
```

or into a community file, as the line from `keygen`, under the
community's name:

```
netleo
* honeybunny fur+Ke3-CM51Akgmq81+fD4WeyVHXMqS08+2SLu7fJa
* evilrabbit JMhQ8icD-cRekgmAl0ylVJFOMaDIPXuEjIm3ajasHIe
```

See [Communities](Communities.md).  A user's key depends on the user
name and password only, not on the community, so a user's line can be
moved to another community as it is.

To change a password, replace the user's line with a new one; to lock a
user out (evilrabbit, or a stolen laptop), delete it.  Then `n3nctl reload`
(or SIGHUP) makes the supernode read its configuration and community file
again; `n3nctl reload_communities` reads only the file.  A reload also
changes the community's header keys, which every edge then gets anew from
its supernode - but a locked out one gets none: it cannot reach the other
edges directly any more either.

With several supernodes, each needs the same users: there is no syncing
built in, so copy the configuration or the file to all of them
(Syncthing, scp from a script, ...), and reload each.  In between, they
do not agree, and the stricter one wins: a supernode passes each
registration on to the others of its federation, and drops the edge when
one of them refuses it.  So

- a user deleted at one supernode is out at all of them at once - its
  edges log `authentication error, username or password not recognized by
  another supernode of the federation` - but only while that supernode is
  up and in the federation: delete the user everywhere, the others need
  not follow at once;
- a new user, or a new password, has to be at every supernode before the
  user's edges come with it: one that does not know it refuses them for
  all.

### 3. The edges

The user name is `connection.description` (at most 15 characters), the
password `auth.password` (or `N3N_PASSWORD` in the environment, to keep
it off the command line), the federation's key `auth.pubkey`:

```ini
[community]
name = netleo
key = mySecretKey
cipher = ChaCha20
supernode = sn.example.org:7654

[connection]
description = honeybunny

[auth]
password = cutie
pubkey = XhnQB26arLouAf3VXjGoAPTjTRIgEJ01Yi30Fhuqc7K
```

Header encryption is switched on by itself, as this scheme builds on it.
Use a stream cipher, ChaCha20 or Speck, with a key: the others are not
safe with this scheme.

Give each edge a user of its own, even though one user can be used by
several edges at once: the management interface then shows who is who,
and the addresses the supernode hands out, which depend on the user
name, are more likely to differ.

### How it works

Header encryption, with this scheme, uses two keys:

- a **static** key, from the community's name, as plain header
  encryption does.  It is used for the registration only
  (REGISTER_SUPER and its answers), which is also signed with a secret
  that only the supernodes of the federation and that one edge share;
- a **dynamic** key, which the supernode makes from the federation's
  name (among others) and hands to an edge it has let in, encrypted with
  the edge's public key.  Everything else - the traffic, the REGISTERs
  between the edges, the pings - is encrypted with it.

An edge that the supernode does not let in never gets the dynamic key,
and cannot take part in anything beyond its attempt to register.  See
[Cryptography](../internals/Crypto.md#header) for the details.
