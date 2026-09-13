//! Small diagnostic errors: no request bodies, credentials, or SDP in messages.
use std::fmt;

#[derive(Debug, Clone, Copy)]
pub(crate) struct Error {
    context: &'static str,
    code: Option<i32>,
}
pub(crate) type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub(crate) fn new(context: &'static str) -> Self {
        Self {
            context,
            code: None,
        }
    }
    pub(crate) fn native(context: &'static str, code: i32) -> Self {
        Self {
            context,
            code: Some(code),
        }
    }
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.context)?;
        if let Some(code) = self.code {
            write!(f, " (code {code})")?;
        }
        Ok(())
    }
}
impl std::error::Error for Error {}
impl From<&'static str> for Error {
    fn from(value: &'static str) -> Self {
        Self::new(value)
    }
}
pub(crate) fn check(code: i32, context: &'static str) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error::native(context, code))
    }
}
