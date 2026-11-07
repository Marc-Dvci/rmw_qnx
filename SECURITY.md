# Security

Please report a vulnerability through
[GitHub's private reporting](https://github.com/Marc-Dvci/rmw_qnx/security/advisories/new), not in a public issue.
The supported version is the `main` branch. There is no hosted service: rmw_qnx is a library and a resource manager
that run inside a QNX system.

What rmw_qnx trusts, and what it does not:

- **Bytes from other processes are not trusted.** CDR from a topic ring, a service request or a reply is read with a
  bounds-checked reader ([`rmw_qnx/core/cdr.hpp`](rmw_qnx/core/cdr.hpp)); a forged sequence length is refused before
  anything is allocated for it. A report of a buffer that crashes `rmw_deserialize`, `rmw_take` or a service is the
  most useful kind.
- **Processes on the same QNX system are trusted to the level of file permissions.** Topic rings live in
  `/dev/shmem` and the service and graph registries in `/tmp/rmwqnx`, created with mode 0666/0777 so any ROS 2
  process can join. A local process can read and write any topic, and register a service under any name. rmw_qnx
  does not implement ROS 2 security (access control, encryption): the enclave and security options are accepted
  and have no effect.
- **Nothing is sent over a network.** Message passing and shared memory are local to one QNX system.

Out of scope: a local process with the same user id deliberately corrupting a ring or a registry file. That is
within what the file permissions above allow.
