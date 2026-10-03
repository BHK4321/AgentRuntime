from pydantic import BaseModel, ConfigDict, Field, field_validator

MAX_BYTES = 1024 * 1024


class ScriptPayload(BaseModel):
    model_config = ConfigDict(extra="forbid")
    input_path: str = Field(min_length=1, max_length=300)
    output_path: str = Field(pattern=r"^output/[a-zA-Z0-9_-]+\.txt$")
    code: str = Field(min_length=1, max_length=32000)

    @field_validator("input_path")
    @classmethod
    def relative_path(cls, value):
        value = value.replace("\\", "/")
        if value.startswith("/") or ":" in value or ".." in value.split("/") or value.startswith(".script-jobs/"):
            raise ValueError("input must stay within the document work directory")
        return value
