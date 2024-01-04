<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Doing Releases for stalld

We multiple places that consume stalld, so to keep our sanity we need
to have a defined procedure for doing a version update. The general
outline for this is:

	1. Tag the gitlab source
	2. Make a gitlab release
	3. Push the code to kernel.org
	4. (optional) Do a Fedora Release
	5. (optional) any other Distro release stuff

## Source Tagging
Our canonical location for source is the [gitlab
repository](https://gitlab.com/rt-linux-tools/stalld) with the
releases happening on branch **main**. The first step toward doing
a release after all the updates are merged into main is to create a
signed git tag, of the form vX.Y.Z, where X is the major release
(currently '1'), Y is a minor release and Z would be used if needed
to perform a quick fix on a minor release. If we update the major or
minor release the Z field should be zero. Ex:

	```bash
	$ git tag -s v1.2.0
	```

In general we won't use the third field very much, so shorthand
references to releases such as the tag above is version 1.2.

## Gitlab Release

Once you've signed the tag with your gpg key, we can move on to the
next step which is doing a gitlab release. That section is entered by
the Releases entry on the left-side menu, then by clicking on the New
Release button in the upper right corner of the page. You input the
appropriate git tag and fill out the Release Title and any Release
Notes required. We currently don't use Milestones or Release assets.

## Kernel.org release

This action may only be performed by a maintainers who has a
kernel.org account. The process is twofold, the first being a simple
push of the source to the kernel.org git repo for stalld. Ensure that
you are pushing from the main branch and that you have a valid remote
for the kernel.org tree. Here's what my kernel.org remote block looks
like:

	```INI
	[remote "kernel.org"]
		url = git@gitolite.kernel.org:pub/scm/utils/stalld/stalld.git
		fetch = +refs/heads/*:refs/remotes/kernel.org/*
		push = +refs/heads/*
		push = +refs/tags/*
		skipDefaultUpdate = true
	```

The url section depends on your having setup
[gitolite](https://korg.docs.kernel.org/gitolite/index.html)
access to kernel.org repositories. Note the addition of the two push
sections, added so that you can push both source and tags in one git
push operation. Finally the *skipDefaultUpdate* entry means that git
remote update will not fetch kernel.org by default.

Note that an additional step using the kernel.org provided script
**git-archive-signer**. This script adds a **git notes** annotation
for the latest tag of the form vX.Y.Z and pushes that to
kernel.org. The receipt of this note triggers automatic generation of
taballs for the stalld source, stored at:

	https://kernel.org/pub/linux/utils/stalld

To make use of the script, add a block similar to the following to
your .git/config file in the stalld git repository:

	```INI
	[archive-signer]
		remote = kernel.org
		tarname = stalld
		usekey = <gpg-keyid>
	```

The remote value should be whatever remote name you use for the
kernel.org stalld repository and the keyid should be your gpg signing
keyid.
