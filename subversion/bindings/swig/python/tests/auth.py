import unittest, os, setup_path

from svn import core

class SubversionAuthTestCase(unittest.TestCase):
  """Test cases for the Subversion auth."""

  def test_open(self):
    baton = core.svn_auth_open([])
    self.assert_(baton is not None)

  def test_set_parameter(self):
    baton = core.svn_auth_open([])
    core.svn_auth_set_parameter(baton, "name", "somedata")
    core.svn_auth_set_parameter(baton, "name", None)

  def test_invalid_cred_kind(self):
    baton = core.svn_auth_open([])
    self.assertRaises(core.SubversionException, 
            lambda: core.svn_auth_first_credentials(
                "unknown", "somerealm", baton))

  def test_credentials_get_username(self):
    def myfunc(realm, maysave, pool):
      self.assertEquals("somerealm", realm)
      username_cred = core.svn_auth_cred_username_t()
      username_cred.username = "bar"
      username_cred.may_save = False
      return username_cred
    baton = core.svn_auth_open([core.svn_auth_get_username_prompt_provider(myfunc, 1)])
    creds = core.svn_auth_first_credentials(
                core.SVN_AUTH_CRED_USERNAME, "somerealm", baton)
    self.assert_(creds is not None)

  def test_credentials_get_simple(self):
    def myfunc(realm, username, may_save, pool):
      self.assertEquals("somerealm", realm)
      simple_cred = core.svn_auth_cred_simple_t()
      simple_cred.username = "mijnnaam"
      simple_cred.password = "geheim"
      simple_cred.may_save = False
      return simple_cred
    baton = core.svn_auth_open([core.svn_auth_get_simple_prompt_provider(myfunc, 1)])
    creds = core.svn_auth_first_credentials(
                core.SVN_AUTH_CRED_SIMPLE, "somerealm", baton)
    self.assert_(creds is not None)

  def test_credentials_get_ssl_client_cert(self):
    def myfunc(realm, may_save, pool):
      self.assertEquals("somerealm", realm)
      ssl_cred = core.svn_auth_cred_ssl_client_cert_t()
      ssl_cred.cert_file = "my-certs-file"
      ssl_cred.may_save = False
      return ssl_cred
    baton = core.svn_auth_open([core.svn_auth_get_ssl_client_cert_prompt_provider(myfunc, 1)])
    creds = core.svn_auth_first_credentials(
                core.SVN_AUTH_CRED_SSL_CLIENT_CERT, "somerealm", baton)
    self.assert_(creds is not None)

  def test_credentials_get_ssl_client_cert_pw(self):
    def myfunc(realm, may_save, pool):
      self.assertEquals("somerealm", realm)
      ssl_cred_pw = core.svn_auth_cred_ssl_client_cert_pw_t()
      ssl_cred_pw.password = "supergeheim"
      ssl_cred_pw.may_save = False
      return ssl_cred_pw
    baton = core.svn_auth_open([core.svn_auth_get_ssl_client_cert_pw_prompt_provider(myfunc, 1)])
    creds = core.svn_auth_first_credentials(
                core.SVN_AUTH_CRED_SSL_CLIENT_CERT_PW, "somerealm", baton)
    self.assert_(creds is not None)

def suite():
    return unittest.makeSuite(SubversionAuthTestCase, 'test')

if __name__ == '__main__':
    runner = unittest.TextTestRunner()
    runner.run(suite())
